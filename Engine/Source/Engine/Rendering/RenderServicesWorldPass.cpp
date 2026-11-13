// RenderServicesWorldPass.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. All files define members of the same
// RenderServices class; shared file-scope helpers live in RenderServicesDetail.h.
#include "Engine/Rendering/RenderServices.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/IRenderFeature.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"
#include "Engine/Rendering/DrawStreamLookupKey.h"
#include "Engine/Rendering/ForwardDrainOrder.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/PropCoverageFloor.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RTShadowMaskService.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowWorldPass.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/ViewExposureHistory.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DrawProducerRegistry.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Camera/CameraDerivation.h"
#include "Rendering/Core/CullingStrategy.h"
#include "Rendering/Core/FrustumCullingStrategy.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/Shader/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/MsaaColorResolvePass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Rendering/Utils/CubeLut/CubeLutFileParser.h"
#include "Rendering/Utils/CubeLut/CubeLutGpuUpload.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Types/StringId.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"

#include "Rendering/Core/ThreadingUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
void AttachExtraColors(RenderGraph::RGPassBuilder& builder,
                       const RenderServices::WorldPassTargetsRG& targets)
{
    for (uint32_t index = 0; index < targets.ExtraColorCount; ++index)
    {
        const auto& extra = targets.ExtraColor[index];
        if (!extra.Texture.IsValid())
            continue;
        if (extra.Resolve.IsValid())
            builder.AttachColorResolve(extra.Location, extra.Texture, extra.Resolve, extra.Ops);
        else
            builder.AttachColor(extra.Location, extra.Texture, extra.Ops);
    }
}

// The reads the forward fog's textures need (forward_fog.glsl): the sky backdrop, bound on every pass
// (BuildPassResourcesRG), and the froxel volume on VolumetricFog-keyword passes.
void DeclareForwardFogReads(RenderGraph::RGPassBuilder& p, const RenderServices::WorldPassTargetsRG& targets,
                            Rendering::MaterialKeyword passKeywords)
{
    if (targets.SkyBackdrop.IsValid())
        p.Read(targets.SkyBackdrop, RenderGraph::RGTextureRead::Sampled);
    if (targets.VolumetricFog.IsValid() && HasKeyword(passKeywords, Rendering::MaterialKeyword::VolumetricFog))
        p.Read(targets.VolumetricFog, RenderGraph::RGTextureRead::Sampled);
    if (targets.VolumetricClouds.IsValid() && HasKeyword(passKeywords, Rendering::MaterialKeyword::VolumetricClouds))
        p.Read(targets.VolumetricClouds, RenderGraph::RGTextureRead::Sampled);
}

// The froxel volume a VolumetricFog-keyword pass samples (ge_volumetricFog): a transient of the frame
// (the integrated grid, or the temporal history the composite reads), so its physical texture exists
// only during preparation.
void BindVolumetricFogVolume(RenderGraph::RGPrepareContext& ctx, RenderGraph::RGTexture volume, SamplerHandle sampler,
                             ResolvedPassResources& res)
{
    if (!volume.IsValid())
        return;
    if (const TextureHandle texture = ctx.GetTexture(volume); texture.IsValid())
        res.Textures.push_back({HashStringId("ge_volumetricFog"), texture, sampler});
}

// Topology for a contributor's colour variant. Mesh-backed contributors carry
// theirs on the registry entry; the procedural ones (terrain, grass, ocean)
// have no mesh and expand an index range into triangles, which is also the
// topology MaterialVariantCook keys their rows with.
Rendering::PrimitiveTopology ContributorTopology(const Rendering::MeshGPUEntry* entry)
{
    return entry ? entry->topology : Rendering::PrimitiveTopology::TriangleList;
}

// Keywords the colour-variant cache is addressed with. The world pass JSON
// always carries Instanced (GPU-scene mesh draws fetch instance data); OR-ing
// that bit onto a mesh-free contributor misses the cooked row
// MaterialVariantCook emits without it (CBT/ocean/grass are not GE_INSTANCED)
// and the cooked-only runtime has nothing to intern but the 1-output base.
Rendering::MaterialKeyword ContributorColorKeywords(Rendering::MaterialKeyword passKeywords,
                                                    Rendering::MaterialKeyword commandKeywords,
                                                    bool meshBacked)
{
    Rendering::MaterialKeyword keywords = passKeywords | commandKeywords;
    if (!meshBacked)
        keywords = keywords & ~Rendering::MaterialKeyword::Instanced;
    return keywords;
}

struct ForwardCommandStorage
{
    std::vector<DrawCommand> Commands;
    std::vector<DrawBindings::BufferEntry> Buffers;
    std::vector<DrawBindings::TextureEntry> Textures;
    std::vector<std::byte> PushConstants;
    std::vector<DrawSubCommand> SubDraws;
};

template <class T>
std::span<const T> CopyForwardSpan(std::vector<T>& storage, std::span<const T> source)
{
    if (source.empty())
        return {};
    const size_t offset = storage.size();
    storage.insert(storage.end(), source.begin(), source.end());
    return {storage.data() + offset, source.size()};
}

std::span<const DrawCommand> CaptureForwardCommands(RenderGraph::RGFrame& frame,
                                                   std::span<const DrawCommand> commands)
{
    if (commands.empty())
        return {};
    auto* storage = frame.AllocatePassData<ForwardCommandStorage>();
    storage->Commands.assign(commands.begin(), commands.end());
    size_t buffers = 0, textures = 0, pushConstants = 0, subDraws = 0;
    for (const auto& command : commands)
    {
        buffers += command.Bindings.Buffers.size();
        textures += command.Bindings.Textures.size();
        pushConstants += command.Bindings.PushConstants.size();
        subDraws += command.SubDraws.size();
    }
    // Reserve complete backing arrays before installing any command span.
    storage->Buffers.reserve(buffers);
    storage->Textures.reserve(textures);
    storage->PushConstants.reserve(pushConstants);
    storage->SubDraws.reserve(subDraws);
    for (auto& command : storage->Commands)
    {
        command.Bindings.Buffers = CopyForwardSpan(storage->Buffers, command.Bindings.Buffers);
        command.Bindings.Textures = CopyForwardSpan(storage->Textures, command.Bindings.Textures);
        command.Bindings.PushConstants = CopyForwardSpan(storage->PushConstants, command.Bindings.PushConstants);
        command.SubDraws = CopyForwardSpan(storage->SubDraws, command.SubDraws);
    }
    return storage->Commands;
}

struct PreparedForwardDraw
{
    const DrawCommand* Source = nullptr;
    PreparedDrawBindings Bindings;
    Rendering::MeshGPUEntryBindings Geometry;
    DrawSubCommand Direct;
    std::optional<uint32_t> ServedVersion;
    uint32_t MaterialIndex = 0;
};

// The command and its spans belong to the frame's declaration snapshot.
std::optional<PreparedForwardDraw> PrepareForwardDrawCommand(
    const DrawCommand& command, uint32_t pipelineSetCount, RenderServices& services,
    const PassBindingContext& pass, Rendering::FrontFace winding, const RenderGraph::RGPrepareContext& context)
{
    const auto& meshRegistry = services.GetMeshGPURegistry();
    auto& variants = services.Materials().Variants();
    auto& binder = services.Materials().Binder();
    const auto* material = command.Material;
    if (!material && (!command.InternedPipeline.IsValid() || !command.PipelineMeta))
        return std::nullopt;

    const Rendering::MeshGPUEntry* meshEntry = nullptr;
    Rendering::MeshGPUEntryBindings meshBindings{};
    Rendering::VertexAttributeFlags effectiveFlags = command.VertexFlags;
    if (command.Geometry.Mesh.IsValid())
    {
        meshEntry = meshRegistry.Find(command.Geometry.Mesh);
        if (!meshEntry || meshEntry->indexCount == 0)
            return std::nullopt;
        if (!meshRegistry.TryGetDrawableBindings(*meshEntry, meshBindings))
            return std::nullopt;
        effectiveFlags = BoundStreamVertexFlags(
            *meshEntry, meshBindings, material && material->IgnoresVertexColor());
    }

    Rendering::PipelineHandle pipeline{};
    if (command.InternedPipeline.IsValid())
        pipeline = context.PreparePipeline(command.InternedPipeline);
    const Rendering::ShaderMeta* variantMeta = command.PipelineMeta;
    std::span<const Rendering::DescriptorSetLayoutId> pipelineSetLayouts;
    const PipelineVariantCache::VariantServeUnit* served = nullptr;
    if (!pipeline.IsValid() && material)
    {
        const Rendering::MaterialKeyword contributorKeywords =
            ContributorColorKeywords(pass.PassKeywords, command.PassKeywords,
                                     meshEntry != nullptr);
        // Contributor draws use the view's base winding (not parity-split).
        auto [servedPipeline, unit] = variants.PrepareColorVariant(
            *material, effectiveFlags, ContributorTopology(meshEntry),
            contributorKeywords, winding, context);
        pipeline = servedPipeline;
        served = unit;
        if (served)
        {
            variantMeta = served->VariantMeta.get();
            pipelineSetLayouts = served->SetLayouts;
        }
    }
    if (!pipeline.IsValid())
        return std::nullopt;

    auto bindings = material
                        ? binder.PrepareMaterialForDraw(pass, *material, pipeline, variantMeta, command.Bindings,
                                                        pipelineSetCount, pipelineSetLayouts)
                        : binder.PreparePipelineForDraw(pass, pipeline, *command.PipelineMeta, command.Bindings,
                                                        pipelineSetCount, pipelineSetLayouts);
    if (!bindings.Pipeline.IsValid())
        return std::nullopt;
    DrawSubCommand direct{command.IndexCount, command.InstanceCount, command.FirstIndex, command.FirstInstance, command.VertexOffset};
    if (meshEntry)
    {
        meshBindings.indexType = meshEntry->indexType;
        if (direct.IndexCount == 0)
        {
            direct.IndexCount = meshEntry->indexCount;
            direct.FirstIndex = meshEntry->firstIndex;
            direct.VertexOffset = static_cast<int32_t>(meshEntry->vertexOffset);
        }
    }
    else
    {
        meshBindings.coreVB = command.Geometry.AltGeom.VB;
        meshBindings.indexBuffer = command.Geometry.AltGeom.IB;
        meshBindings.indexType = static_cast<uint32_t>(command.Geometry.AltGeom.IndexType);
    }

    PreparedForwardDraw result{&command, bindings, meshBindings, direct, {}, 0};
    if (served)
    {
        result.MaterialIndex = material->GetGpuSceneMaterialIndex();
        result.ServedVersion = served->MaterialVersion;
    }
    return result;
}

void RecordForwardDrawCommand(const PreparedForwardDraw& prepared, RenderServices& services,
                              PassBindingContext& pass, DrawGeometryBindings& geometryBindings)
{
    const auto& command = *prepared.Source;
    const auto& mesh = prepared.Geometry;
    geometryBindings.BindVertexBuffer(mesh.coreVB, 0);
    geometryBindings.BindVertexBuffer(mesh.tangentVB, 1);
    geometryBindings.BindVertexBuffer(mesh.colorVB, 2);
    geometryBindings.BindVertexBuffer(mesh.uv1VB, 3);
    geometryBindings.BindVertexBuffer(mesh.jointsVB, 4);
    geometryBindings.BindVertexBuffer(mesh.weightsVB, 5);
    geometryBindings.BindVertexBuffer(mesh.joints1VB, 6);
    geometryBindings.BindVertexBuffer(mesh.weights1VB, 7);
    for (uint32_t streamIndex = 0; streamIndex < mesh.extraUvVB.size(); ++streamIndex)
        geometryBindings.BindVertexBuffer(mesh.extraUvVB[streamIndex], 8u + streamIndex);
    geometryBindings.BindIndexBuffer(mesh.indexBuffer, static_cast<Rendering::IndexType>(mesh.indexType));
    services.Materials().Binder().BindPreparedForDraw(pass, prepared.Bindings, command.Bindings.PushConstants);

    if (command.UseIndirect && command.IndirectCommandBuffer.IsValid())
    {
        pass.Cmd->DrawIndexedIndirectCount(
            command.IndirectCommandBuffer,
            command.IndirectCountBuffer,
            command.IndirectMaxDrawCount,
            command.IndirectStride,
            command.IndirectCommandOffset,
            command.IndirectCountOffset);
    }
    else if (command.SubDraws.empty())
    {
        pass.Cmd->DrawIndexed(prepared.Direct.IndexCount, prepared.Direct.InstanceCount, prepared.Direct.FirstIndex,
                              prepared.Direct.VertexOffset, prepared.Direct.FirstInstance);
    }
    else
    {
        for (const auto& sub : command.SubDraws)
        {
            pass.Cmd->DrawIndexed(sub.IndexCount, sub.InstanceCount,
                                  sub.FirstIndex, sub.VertexOffset, sub.FirstInstance);
        }
    }
    if (prepared.ServedVersion)
        services.Materials().Variants().RecordServedGeneration(
            prepared.MaterialIndex, ServedPassClass::Color, *prepared.ServedVersion);
}

// The DDGI atlases and probe state a world pass samples through the bindings
// BuildPassResourcesRG adds under the DDGI keyword, imported for the pass to
// declare. DDGIGen writes them earlier in the frame and the bindings alone
// form no edge; at GlossyResolveScale::Full no resolve sits between them and
// the pass. Gated like those bindings: empty without the keyword or the feature.
DDGIProbeFeature::GatherReadsRG ImportDdgiGatherReads(RenderGraph::RGFrame& frame, DDGIProbeFeature* ddgi,
                                                      Rendering::MaterialKeyword passKeywords)
{
    if (!HasKeyword(passKeywords, Rendering::MaterialKeyword::DDGI) || !ddgi || !ddgi->IsInitialized())
        return {};
    return ddgi->ImportGatherReads(frame);
}

void DeclareDdgiGatherReads(RenderGraph::RGPassBuilder& p, const DDGIProbeFeature::GatherReadsRG& reads)
{
    for (const RenderGraph::RGTexture& atlas : reads.Atlases)
        if (atlas.IsValid())
            p.Read(atlas, RenderGraph::RGTextureRead::Sampled);
    for (const RenderGraph::RGBuffer& probeState : reads.ProbeStates)
        if (probeState.IsValid())
            p.Read(probeState, RenderGraph::RGBufferRead::Storage);
}

// --- Distant-prop coverage census (GE_PROP_CENSUS=1; diagnostic, default off) ---
// Answers "how many frustum-visible instances would a scatter coverage floor
// cull?" — the sizing evidence that decides whether that cull machinery is
// worth building for a scene (2026-07-20 ElvenRealm verdict: no, 0.01-0.20%
// at the gate poses). CPU twin of the scatter's coverage math
// (PropCoverageFloor.h::CoverageNdc — unbiased, pre-lodBias). World-space
// frustum test only: the camera-relative origin refinement is skipped, which
// is exact at sector-0 scenes and a diagnostic approximation elsewhere; the
// count also ignores HZB occlusion, so it upper-bounds the scatter's actual
// candidate set on occlusion-culled views.
bool PropCensusEnabled()
{
    static const bool kEnabled = Platform::EnvironmentSwitchEnabled("GE_PROP_CENSUS", false);
    return kEnabled;
}

void LogPropCoverageCensus(const Rendering::GPUScene& gpuScene,
                           const Rendering::CameraData& cam, uint32_t viewId,
                           uint32_t viewportW, uint32_t viewportH, uint32_t worldKey)
{
    const float projScaleY = (cam.cameraPos[3] == 1.0f) ? 0.0f : std::abs(cam.proj.Data()[5]);
    if (projScaleY <= 0.0f || viewportH == 0u)
        return;

    // The census runs in the frame of the camera's render origin sector: viewProjRel planes,
    // each sector-local center composed against the sector.
    const Mathematics::Vector3Int originSector = CameraRenderOriginSector(cam);
    const Mathematics::Vector3 eye = CameraEyeRelativeToOrigin(cam);
    Rendering::Vector4 planes[6];
    Rendering::ExtractFrustumPlanes(cam.viewProjRel, planes);

    uint32_t visible = 0, subFloor = 0, subAdmit = 0, sub1 = 0, sub2 = 0, sub4 = 0, sub8 = 0;
    const float floorNdc = Rendering::CoverageFloorNdc(
        Rendering::kMinPropCoveragePx, Rendering::kCoverageFloorReferenceHeightPx);
    const float admitNdc =
        Rendering::CoverageFloorAdmitNdc(floorNdc, Rendering::kCoverageFloorHysteresis);
    for (const Rendering::GPUInstance& inst : gpuScene.GetInstances())
    {
        if (inst.meshIndex == 0xFFFFFFFFu)
            continue; // tombstone
        if (worldKey != 0u && ((inst.flags >> 16u) & 0xFFFFu) != worldKey)
            continue;
        const Rendering::Vector3 center = Rendering::SectorLocalPointRelativeTo(
            inst.boundingCenter, inst.sectorPacked[0], inst.sectorPacked[1], originSector);
        if (!Rendering::TestSphereFrustum(center, inst.boundingRadius, planes))
            continue;
        ++visible;
        const float dx   = eye.x - center.x;
        const float dy   = eye.y - center.y;
        const float dz   = eye.z - center.z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float covNdc = Rendering::CoverageNdc(inst.boundingRadius, projScaleY, dist);
        // The recommended floor's candidate set (base threshold + the band top).
        if (covNdc < floorNdc)
            ++subFloor;
        if (covNdc < admitNdc)
            ++subAdmit;
        // Device-pixel context at this viewport height.
        const float covPx = covNdc * static_cast<float>(viewportH);
        if (covPx < 1.0f)
            ++sub1;
        if (covPx < 2.0f)
            ++sub2;
        if (covPx < 4.0f)
            ++sub4;
        if (covPx < 8.0f)
            ++sub8;
    }
    if (visible == 0u)
        return;
    Logger::Log::Info(
        "[PropCensus] view={} vp={}x{} visible={} | floor@{}px/1080ref: below={} ({:.2f}%) "
        "inBandTop={} ({:.2f}%) | actualPx <1:{} <2:{} <4:{} <8:{}",
        viewId, viewportW, viewportH, visible, Rendering::kMinPropCoveragePx, subFloor,
        100.0 * subFloor / visible, subAdmit, 100.0 * subAdmit / visible, sub1, sub2, sub4,
        sub8);
}

struct PreparedWorldDraw
{
    DrawCommand Command;
    PreparedForwardDraw Prepared;
    size_t PushConstantOffset = 0;
    size_t PushConstantSize = 0;
    bool Entity = false;
    uint32_t MaterialIndex = 0;
    uint32_t MeshIndex = 0;
    uint32_t MaxDrawCount = 0;
    size_t RangeCount = 0;
};
struct WorldPassData
{
    PassBindingContext Inputs;
    std::span<const DrawCommand> Commands;
    std::vector<PreparedWorldDraw> Draws;
    std::vector<std::byte> PushConstants;
    size_t BaseTextureCount = 0;
    RenderGraph::RGTexture VolumetricFog{};
    SamplerHandle FogSampler{};
    bool HasAnyDraws = false;
    size_t EntityBatchCount = 0;
    bool Consolidated = false;
    uint32_t LiveGroupCount = 0;
};
struct WorldBatchPreparation
{
    const WorldDrawBuilder::BatchKey& Key;
    const Engine::Renderer::Material* Material;
    const Rendering::MeshGPUEntry* Mesh;
    Rendering::MeshGPUEntryBindings Geometry;
    Rendering::VertexAttributeFlags VertexFlags;
    Rendering::MaterialKeyword Keywords;
    Rendering::GPUDrawStreamBuilder::BatchDrawRange Range;
    DrawBindings Bindings;
    uint64_t IndirectionAddress;
};

// The view's ViewParams and ExposureHistory that the pipeline publishes this frame, as the MSAA color
// resolve's inputs (Pipeline::MsaaResolveViewBuffersFor).
Passes::MsaaResolveViewBuffers PipelineMsaaResolveViewBuffers(const Pipeline::RenderPipelineInstance* instance,
                                                              const RenderGraph::RGFrame& frame, ViewId viewId,
                                                              BufferHandle placeholder)
{
    Pipeline::PipelineBufferBindingRG viewParams{};
    Pipeline::PipelineBufferBindingRG exposureHistory{};
    if (const Pipeline::PipelineFrameResources* resources = instance ? instance->FrameResourcesFor(&frame) : nullptr)
        resources->ForEachBufferBinding(
            viewId,
            [&viewParams, &exposureHistory](StringId nameId, const Pipeline::PipelineBufferBindingRG& binding)
            {
                constexpr StringId kViewParams = HashStringId(Pipeline::Names::Res::ViewParams);
                constexpr StringId kExposureHistory = HashStringId(Pipeline::Names::Res::ExposureHistory);
                if (nameId == kViewParams)
                    viewParams = binding;
                else if (nameId == kExposureHistory)
                    exposureHistory = binding;
            });
    return Pipeline::MsaaResolveViewBuffersFor(viewParams, exposureHistory, placeholder);
}


} // namespace

ResolvedPassResources RenderServices::BuildPassResourcesRG(
    Rendering::RenderGraph::RGFrame& frame,
    Rendering::ViewId viewId,
    Rendering::MaterialKeyword passKeywords,
    const WorldPassTargetsRG* targets,
    Rendering::BufferHandle camBuf,
    uint64_t camOffset)
{
    using namespace ::GameEngine::Rendering;

    ResolvedPassResources resources;
    if (!m_Device)
        return resources;
    // Pre-size to the worst-case full-keyword path: avoids growth reallocations and amortizes
    // the per-insert linear dedup scans (every depth/world pass declaration builds this table).
    resources.Buffers.reserve(24);
    resources.Textures.reserve(16);

    // Same upsert semantics as ResolvePassResources: last add wins per name.
    auto add = [&](StringId nameId, BufferHandle h, uint64_t offset = 0, uint64_t size = 0)
    {
        if (!h.IsValid())
            return;
        for (auto& e : resources.Buffers)
        {
            if (e.Name == nameId)
            {
                e = {nameId, h, offset, size};
                return;
            }
        }
        resources.Buffers.push_back({nameId, h, offset, size});
    };
    auto addTexture = [&](StringId nameId, TextureHandle texture, SamplerHandle sampler)
    {
        if (!texture.IsValid() || !sampler.IsValid())
            return;
        for (auto& e : resources.Textures)
        {
            if (e.Name == nameId)
            {
                e = {nameId, texture, sampler};
                return;
            }
        }
        resources.Textures.push_back({nameId, texture, sampler});
    };

    for (const auto& [nameId, buf] : m_FallbackBuffers)
        add(nameId, buf);

    if (const auto* pv = m_ViewRegistry.FindPerView(viewId))
    {
        const auto lightBuffer = pv->CurrentLightBuffer();
        if (lightBuffer.IsValid())
        {
            add(HashStringId("LightUBO"), lightBuffer);
            add(HashStringId("Light"),    lightBuffer);
        }
    }

    // Frame-guarded probe: an entry written by a different frame stream (or a
    // stale incarnation of this one) holds dead frame-local ids — treat as
    // absent. (Frame, FrameIndex), never pointer alone.
    const ViewFrameRG* vfr = FindViewFrameRGFor(frame, viewId);

    // AreaShadowData: the area arm's same-frame upload alloc (it was written
    // at that arm's declaration; the entry exists only when the arm declared).
    if (vfr && vfr->AreaShadowData.Valid())
    {
        add(HashStringId("AreaShadowData"), vfr->AreaShadowData.Buffer,
            vfr->AreaShadowData.Offset, sizeof(AreaShadowDataGPU));
    }
    if (vfr && vfr->SpotShadowData.Valid())
    {
        add(HashStringId("SpotShadowData"), vfr->SpotShadowData.Buffer,
            vfr->SpotShadowData.Offset, sizeof(SpotShadowDataGPU));
    }
    // MotionParams: the motion variant's per-view block, written at the
    // producer arm's declaration. Bound by the block's reflected INSTANCE
    // name; the zero fallback above covers every view whose producer did not
    // run, so the binding exists on every draw either way.
    if (vfr && vfr->DeformationMotion.Valid())
    {
        add(HashStringId(kDeformationMotionParamsName), vfr->DeformationMotion.Buffer,
            vfr->DeformationMotion.Offset, sizeof(DeformationMotionParamsGPU));
    }
    // MotionHistory: the scatter's per-instance rendered level and phase
    // history for this frame, the channel the motion variant reads validity
    // from. Present only while a view asks for the history, which is only
    // while a producer arm is enabled — and the motion variant is the only
    // program that declares the binding, so nothing else is affected by its
    // absence.
    if (m_DrawStreamBuilder)
    {
        const auto history =
            m_DrawStreamBuilder->GetRenderedHistoryForRead(static_cast<uint32_t>(viewId));
        if (history.IsValid())
            add(HashStringId("MotionHistory"), history.Buffer, 0, history.Bytes);
    }
    if (vfr && vfr->PointShadowData.Valid())
    {
        // M1: variable-length std430 slot array (budget * sizeof(PointShadowSlotGPU)).
        add(HashStringId("PointShadowData"), vfr->PointShadowData.Buffer,
            vfr->PointShadowData.Offset, vfr->PointShadowData.Bytes);
    }

    // Pipeline buffer bindings from the frame blackboard (the RenderGraph replacement
    // for ForEachBufferRef). Same upsert position as the old block: after the
    // fallbacks so pipeline entries OVERRIDE the fallback ShadowData/
    // ViewParams names, before MaterialParams/Cam. Upload allocs arrive as
    // {Buffer, Offset, Size}; device-local pool imports as whole-buffer
    // physicals.
    if (auto* instance = m_FrameOrchestrator.PipelineInstanceForFrame(frame))
    {
        if (const auto* fr = instance->FrameResourcesFor(&frame))
        {
            fr->ForEachBufferBinding(
                viewId, [&](StringId nameId, const Pipeline::PipelineBufferBindingRG& b)
                { add(nameId, b.Buffer, b.Offset, b.Size); });
        }
    }

    if (vfr && vfr->DirectionalShadowData.Valid())
        add(HashStringId("ShadowData"), vfr->DirectionalShadowData.Buffer,
            vfr->DirectionalShadowData.Offset, sizeof(ShadowDataGPU));

    if (const auto materialParams = m_MaterialSystem.PackedMaterialParams();
        materialParams.Buffer.IsValid())
        add(HashStringId("MaterialParams"), materialParams.Buffer,
            materialParams.Offset, materialParams.Size);

    if (auto paletteBuf = GetSkinPaletteAtlas().GetBuffer(); paletteBuf.IsValid())
        add(HashStringId("BonePaletteAtlas"), paletteBuf);

    // Compatibility profile: the vertex stage reads GPUScene instances through a
    // set-0 SSBO instead of a device address, by way of this view's instance
    // index list (instance_io.glsl). Bound for every pass because entity draws
    // exist in both the colour and depth families; a view that draws nothing
    // keeps the fallback list.
    if (m_Profile.IsCompat())
    {
        if (auto* gpuScene = GetGPUScene(); gpuScene && gpuScene->GetInstanceBuffer().IsValid())
            add(HashStringId("GPUInstances"), gpuScene->GetInstanceBuffer());
        const ViewFrameRG::UploadedBuffer list = UploadCompatInstanceList(frame, viewId);
        if (list.Valid())
            add(HashStringId(CpuDrawStreamBuilder::kIndexListBindingName), list.Buffer, list.Offset, list.Bytes);
    }

    if (camBuf.IsValid())
        add(HashStringId("Cam"), camBuf, camOffset, sizeof(CameraData));

    {
        // ge_sceneDepth: prefer the single-sample resolved depth so materials
        // sampling it as sampler2D work at any MSAA level. Targets must be
        // imports (pool/external) — PhysicalTexture asserts on a transient,
        // which is the documented constraint of this declaration-time table.
        //
        // The last resort is the area/spot shadow fallback, shared for its
        // shape (1x1 D32) and, more importantly, its VALUE: its texel is
        // reverse-Z FAR, which reads as "no opaque surface in front of me" —
        // the no-op every consumer of this binding wants. Depth passes pass a
        // null `targets` and land here by design.
        //
        // Consumers texelFetch this by SCREEN coordinate, so on the 1x1
        // fallback only (0,0) is in bounds; the rest are out-of-range fetches,
        // which no robustness feature is enabled to define. Keep any new
        // consumer clamped to textureSize(ge_sceneDepth, 0), not to
        // ge_screenSize.
        TextureHandle sceneDepth = m_AreaShadowFallbackTexture;
        if (targets)
        {
            const Rendering::RenderGraph::RGTexture depthTex =
                targets->DepthResolved.IsValid() ? targets->DepthResolved : targets->Depth;
            if (depthTex.IsValid())
            {
                const TextureHandle viewDepth = frame.PhysicalTexture(depthTex);
                if (viewDepth.IsValid())
                    sceneDepth = viewDepth;
            }
        }
        addTexture(HashStringId("ge_sceneDepth"), sceneDepth,
                   m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp));
    }

    // ge_gtao: the screen-space GTAO result (rgb world bent normal, a visibility). Bound
    // only on GTAO-keyword passes (set when an AO volume is active for the view), so
    // non-AO variants never declare or expect binding 27. The fallback is the (0,0,0,1)
    // black texture — a=1 is full visibility (a no-op), rgb=0 the "no bent normal"
    // sentinel — kept defensively for the rare frame the keyword is set but the AO node
    // has not published yet.
    if (HasKeyword(passKeywords, MaterialKeyword::GTAO))
    {
        TextureHandle gtao = m_Textures->GetDefaultBlackTexture();
        if (targets && targets->GTAO.IsValid())
        {
            const TextureHandle viewGtao = frame.PhysicalTexture(targets->GTAO);
            if (viewGtao.IsValid())
                gtao = viewGtao;
        }
        addTexture(HashStringId("ge_gtao"), gtao,
                   m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp));
    }

    // ge_skyBackdrop: what the sky pass draws its backdrop from, for the height fog's colour taken
    // from the sky (forward_fog.glsl). Bound whatever the keywords: the terrain and the grass draw in
    // this pass with the keywords their nodes chose, and every set this pass hands out must carry it.
    // Black without a sky; the fog reads it only when its colour source names the sky.
    if (targets)
    {
        TextureHandle backdrop = m_Textures->GetDefaultBlackTexture();
        if (targets->SkyBackdrop.IsValid())
        {
            const TextureHandle viewBackdrop = frame.PhysicalTexture(targets->SkyBackdrop);
            if (viewBackdrop.IsValid())
                backdrop = viewBackdrop;
        }
        addTexture(HashStringId("ge_skyBackdrop"), backdrop,
                   m_Textures->GetSampler(Rendering::SamplerPreset::LinearRepeat));
    }

    // ge_volumetricFogIrradiance: the IBL irradiance cube the volumetric fog lights its continuation
    // past the volume with (volumetric_fog_forward.glsl), as its composite does. The volume itself
    // is a transient of the frame, bound during preparation (BindVolumetricFogVolume).
    if (HasKeyword(passKeywords, MaterialKeyword::VolumetricFog))
    {
        TextureHandle irradiance = m_Textures->GetDefaultBlackCubeTexture();
        SamplerHandle cubeSampler = m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp);
        if (auto* ibl = GetFeature<ImageBasedLightingFeature>(); ibl && ibl->IsInitialized())
        {
            irradiance = ibl->GetIrradianceCube();
            cubeSampler = ibl->GetCubeSampler();
        }
        addTexture(HashStringId("ge_volumetricFogIrradiance"), irradiance, cubeSampler);
    }

    // ge_volumetricClouds: the view's resolved clouds (a pool texture of the graph), for the blended
    // surfaces drawn after the clouds composite that stand beyond them (forward_fog.glsl).
    if (HasKeyword(passKeywords, MaterialKeyword::VolumetricClouds) && targets && targets->VolumetricClouds.IsValid())
    {
        const TextureHandle clouds = frame.PhysicalTexture(targets->VolumetricClouds);
        addTexture(HashStringId("ge_volumetricClouds"), clouds.IsValid() ? clouds : m_Textures->GetDefaultWhiteTexture(),
                   m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp));
    }

    // ge_rtShadowMask: the ray-query directional shadow mask (R8, 1 = lit).
    // Bound only on RTShadowMask-keyword passes (set when the RT lane produced
    // a mask for the view this frame), so non-RT variants never declare or
    // expect binding 28. The fallback is the 1x1 WHITE texture — fully lit,
    // i.e. "no shadow", never garbage — kept defensively for a pass whose
    // keyword is set but whose physical import failed.
    if (HasKeyword(passKeywords, MaterialKeyword::RTShadowMask))
    {
        TextureHandle mask = m_Textures->GetDefaultWhiteTexture();
        if (vfr && vfr->RTShadowMask.IsValid())
        {
            const TextureHandle viewMask = frame.PhysicalTexture(vfr->RTShadowMask);
            if (viewMask.IsValid())
                mask = viewMask;
        }
        addTexture(HashStringId("ge_rtShadowMask"), mask,
                   m_Textures->GetSampler(Rendering::SamplerPreset::PointClamp));
    }

    if (HasKeyword(passKeywords, MaterialKeyword::ScreenSpaceShadows) &&
        vfr && vfr->ScreenSpaceShadowMask.IsValid())
    {
        // R32_UINT: the keyword is armed only after the service has validated
        // this physical import. A normalized-colour fallback is not type-compatible.
        addTexture(HashStringId("ge_screenSpaceShadowMask"),
                   frame.PhysicalTexture(vfr->ScreenSpaceShadowMask),
                   m_Textures->GetSampler(Rendering::SamplerPreset::PointClamp));
    }

    // ge_sceneColor: the post-opaque scene-colour grab the transmissive pass refracts. Bound
    // only on SceneColorGrab-keyword passes (the dedicated transmissive pass, and only when the
    // grab actually ran), so non-glass / inline variants never declare or expect binding 22. The
    // fallback is a 1x1 black COLOUR texture (type/layout-compatible with the sampler2D), never the
    // live SceneColor — it just keeps the descriptor valid defensively.
    if (HasKeyword(passKeywords, MaterialKeyword::SceneColorGrab))
    {
        TextureHandle sceneColor = m_Textures->GetDefaultBlackTexture();
        if (targets && targets->SceneGrab.IsValid())
            sceneColor = frame.PhysicalTexture(targets->SceneGrab);
        addTexture(HashStringId("ge_sceneColor"), sceneColor,
                   m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp));
    }

    if (HasKeyword(passKeywords, MaterialKeyword::Shadows))
    {
        const StringId kShadowMapName        = HashStringId("ge_shadowMapArray");
        const StringId kShadowMomentsName    = HashStringId("ge_shadowMomentsArray");
        const StringId kAreaShadowMapName    = HashStringId("ge_areaShadowMap");
        const StringId kAreaShadowMapRawName = HashStringId("ge_areaShadowMapRaw");
        const StringId kSpotShadowMapName    = HashStringId("ge_spotShadowMap");
        const StringId kPointShadowMapName   = HashStringId("ge_pointShadowMap");

        // Prefer this frame's pool import — the SAME physical the cascade arm
        // attached. The feature cache is the second door to the same physical
        // (frame-stamped, so it reports absence rather than a handle whose image
        // the pool may have freed), and the typed 1x1 fallback closes the gap so
        // shadowed materials never sample an unwritten descriptor.
        auto* feature = GetFeature<ShadowMapRenderFeature>();
        TextureHandle shadowTex{};
        if (vfr && vfr->ShadowMapArray.IsValid())
            shadowTex = frame.PhysicalTexture(vfr->ShadowMapArray);
        if (!shadowTex.IsValid() && feature && feature->IsInitialized())
            shadowTex = feature->GetShadowMapTexture(viewId, frame);

        if (!shadowTex.IsValid())
        {
            // No cascade array for this view this frame. When none was owed that
            // is ordinary authoring — a terrain scene with nothing casting, an
            // area-lit scene with no casting directional, a thumbnail camera
            // before its first update — and a fully-lit fallback is the right
            // answer, silently. When one WAS owed the arm committed to importing
            // and then produced nothing, so the absence is a producer bug:
            // reported in ALL configs, because GE_DEV_DIAG is undefined in
            // Release and an assert-only diagnostic would ship mute.
            if (vfr && vfr->CascadeArrayOwed)
            {
                const auto health = m_Device ? m_Device->GetDeviceHealth()
                                             : Rendering::DeviceHealth::Healthy;
                if (health == Rendering::DeviceHealth::Healthy)
                {
                    Logger::Log::Error(
                        "[Shadows] view {}: the cascade arm committed to importing but no array is "
                        "live for this frame; binding the 1x1 fallback (everything reads lit). A "
                        "producer/declaration-order bug",
                        static_cast<uint32_t>(viewId));
#if defined(GE_DEV_DIAG)
                    assert(false && "cascade array missing after the arm committed to importing");
#endif
                }
                else
                {
                    // Rebuilding / AwaitingReprovision: a transient the recovery
                    // path must survive, not a producer bug.
                    Logger::Log::Warning(
                        "[Shadows] view {}: no cascade array during device recovery ({}); binding "
                        "the 1x1 fallback",
                        static_cast<uint32_t>(viewId),
                        Rendering::DeviceHealthToString(health));
                }
            }
            shadowTex = m_CascadeShadowFallbackTexture;
        }

        // ge_shadowMapArray is declared STATICALLY by every Shadows-keyword
        // receiver, so binding 9 must be written on every such pass or the
        // descriptor keeps the recycled set's previous occupant (the VUID-08114
        // family). The keyword is blueprint-driven and never consults the shadow
        // feature, so a Shadows pass can run with no ShadowMap node at all: both
        // sampler sources above are feature-owned and absent on exactly that
        // path. RenderServices owns the last resort so the pair is unconditional.
        SamplerHandle shadowSampler{};
        if (const auto* shadowRes = m_ViewRegistry.GetViewShadowResources(viewId))
            shadowSampler = shadowRes->shadowSampler;
        if (!shadowSampler.IsValid() && feature && feature->IsInitialized())
            shadowSampler = feature->GetShadowSampler();
        if (!shadowSampler.IsValid())
            shadowSampler = GetCascadeShadowSampler();
        if (shadowTex.IsValid() && shadowSampler.IsValid())
        {
            addTexture(kShadowMapName, shadowTex, shadowSampler);
            if (GetProfile().IsCompat())
                addTexture(HashStringId("ge_shadowMapRaw"), shadowTex, shadowSampler);
        }
        else
        {
            // Both operands are engine-owned device defaults by this point, so
            // reaching here means provisioning itself failed. The binder fails
            // OPEN on an entry it cannot resolve — it skips the slot with a
            // warning that does not name this cause — so say it here.
            Logger::Log::Error(
                "[Shadows] view {}: ge_shadowMapArray cannot be bound (texture {}, sampler {}); "
                "binding 9 will be left unwritten. The cascade fallback resources failed to "
                "provision — see CreateShadowFallbackResources",
                static_cast<uint32_t>(viewId), shadowTex.IsValid() ? "ok" : "MISSING",
                shadowSampler.IsValid() ? "ok" : "MISSING");
            assert(false && "cascade shadow fallback texture/sampler not provisioned");
        }

        if (feature && feature->IsInitialized())
        {
            auto momentsTex = vfr && vfr->MsmMoments.IsValid()
                ? frame.PhysicalTexture(vfr->MsmMoments)
                : feature->GetMsmMomentsTextureForBinding(viewId);
            auto momentsSampler = feature->GetMsmSampler();
            if (momentsTex.IsValid() && momentsSampler.IsValid())
                addTexture(kShadowMomentsName, momentsTex, momentsSampler);
        }

        // Glass transmittance cascade (translucent shadows). The real per-cascade tint
        // array exists only for views with glass casters; otherwise bind a 1x1 white
        // array so the receiver's tint sample is a no-op. Plain linear sampler (it's a
        // colour array, not depth-compare).
        const StringId kTransmittanceName = HashStringId("ge_transmittanceShadowArray");
        TextureHandle tintTex{};
        if (vfr && vfr->TransmittanceShadowArray.IsValid())
            tintTex = frame.PhysicalTexture(vfr->TransmittanceShadowArray);
        if (!tintTex.IsValid())
            tintTex = m_TransmittanceShadowFallback;
        if (tintTex.IsValid())
            addTexture(kTransmittanceName, tintTex,
                       m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp));

        TextureHandle areaShadowTex{};
        if (vfr && vfr->AreaShadowMap.IsValid())
            areaShadowTex = frame.PhysicalTexture(vfr->AreaShadowMap);
        if (!areaShadowTex.IsValid())
            areaShadowTex = m_AreaShadowFallbackTexture;
        auto areaShadowSampler = GetAreaShadowSampler();
        if (areaShadowTex.IsValid() && areaShadowSampler.IsValid())
            addTexture(kAreaShadowMapName, areaShadowTex, areaShadowSampler);
        auto areaShadowRawSampler = GetAreaShadowRawSampler();
        if (areaShadowTex.IsValid() && areaShadowRawSampler.IsValid())
            addTexture(kAreaShadowMapRawName, areaShadowTex, areaShadowRawSampler);

        TextureHandle spotShadowTex{};
        if (vfr && vfr->SpotShadowMap.IsValid())
            spotShadowTex = frame.PhysicalTexture(vfr->SpotShadowMap);
        if (!spotShadowTex.IsValid())
            spotShadowTex = m_AreaShadowFallbackTexture;
        auto spotShadowSampler = GetSpotShadowSampler();
        if (spotShadowTex.IsValid() && spotShadowSampler.IsValid())
            addTexture(kSpotShadowMapName, spotShadowTex, spotShadowSampler);

        TextureHandle pointShadowTex{};
        if (vfr && vfr->PointShadowMap.IsValid())
            pointShadowTex = frame.PhysicalTexture(vfr->PointShadowMap);
        if (!pointShadowTex.IsValid())
            pointShadowTex = m_PointShadowFallbackTexture;
        auto pointShadowSampler = GetPointShadowSampler();
        if (pointShadowTex.IsValid() && pointShadowSampler.IsValid())
            addTexture(kPointShadowMapName, pointShadowTex, pointShadowSampler);
    }

    // Image-based lighting: bind the engine-shared environment cubes + BRDF LUT +
    // EnvData UBO by their reflected GLSL names (see ibl.glsl). The feature seeds
    // always-valid ambient-fallback cubes in Initialize, so addTexture's
    // invalid-handle early-out only skips before the feature is initialized (the
    // IBLGen node initializes it ahead of the world pass). The EnvData reflected
    // name is the instance "Env"; a zero-filled "Env" fallback (registered with
    // the other fallback buffers) covers the pre-init frame.
    if (HasKeyword(passKeywords, MaterialKeyword::IBL))
    {
        if (auto* ibl = GetFeature<ImageBasedLightingFeature>(); ibl && ibl->IsInitialized())
        {
            if (auto* iblSource = ibl->GetEnvironmentSource(); iblSource && iblSource->HasActiveContent())
                ibl->SetIblIntensity(std::max(0.0f, iblSource->IblIntensity()));
            else
                ibl->SetIblIntensity(0.0f);
            // A reflection-probe capture with CaptureEnvironment off binds the
            // zero-filled fallback EnvData (iblIntensity 0) instead of the live
            // one, so the capture faces read no environment radiance and cannot
            // feed the cube they are baking back into themselves. EnvData is a
            // single per-frame ring element shared by every pass, so this must
            // be a SEPARATE buffer, not a re-upload — the last upload would win
            // for all passes. The cubes still bind to satisfy the descriptor
            // layout; iblIntensity 0 zeroes their contribution.
            const BufferHandle envData = m_WorldPassExcludeEnvironment
                                             ? m_FallbackBuffers[HashStringId("Env")]
                                             : ibl->UploadEnvData(m_Device);

            addTexture(HashStringId("ge_irradianceCube"), ibl->GetIrradianceCube(), ibl->GetCubeSampler());
            addTexture(HashStringId("ge_prefilterCube"), ibl->GetPrefilterCube(), ibl->GetCubeSampler());
            addTexture(HashStringId("ge_brdfLUT"), ibl->GetBrdfLut(), ibl->GetLutSampler());
            // Bindings and graph dependencies share this path for entity draws
            // and custom contributors, including views containing only contributors.
            auto& vfr = ViewFrameRGFor(frame, viewId);
            if (!vfr.IblIrradiance.IsValid())
            {
                vfr.IblIrradiance = frame.ImportExternalTexture(
                    "IBL_Irradiance", ibl->GetIrradianceCube(), Rendering::ResourceState::ShaderResource,
                    Rendering::TextureFormat::R16G16B16A16_FLOAT, 1,
                    ImageBasedLightingFeature::kNumCaptureFaces);
                vfr.IblPrefilter = frame.ImportExternalTexture(
                    "IBL_Prefilter", ibl->GetPrefilterCube(), Rendering::ResourceState::ShaderResource,
                    Rendering::TextureFormat::R16G16B16A16_FLOAT, ibl->GetPrefilterMipCount(),
                    ImageBasedLightingFeature::kNumCaptureFaces);
                vfr.IblBrdfLut = frame.ImportExternalTexture(
                    "IBL_BrdfLut", ibl->GetBrdfLut(), Rendering::ResourceState::ShaderResource,
                    Rendering::TextureFormat{}, 1, 1);
            }
            // Compat-profile spellings: ibl.glsl's compat arm declares the trio
            // as separate images over one standalone sampler (WebGPU's
            // per-stage sampler budget). Registered unconditionally — the
            // binder resolves by reflected name, so the unused spelling is
            // never consumed.
            addTexture(HashStringId("ge_irradianceCubeTex"), ibl->GetIrradianceCube(), ibl->GetCubeSampler());
            addTexture(HashStringId("ge_prefilterCubeTex"), ibl->GetPrefilterCube(), ibl->GetCubeSampler());
            addTexture(HashStringId("ge_brdfLUTTex"), ibl->GetBrdfLut(), ibl->GetCubeSampler());
            addTexture(HashStringId("ge_iblSampler"), ibl->GetBrdfLut(), ibl->GetCubeSampler());
            add(HashStringId("Env"), envData);
        }
    }

    // DDGI: bind the engine-shared probe atlas + volume UBO by their
    // reflected GLSL names (Includes/ddgi_probes.glsl). Bound only on
    // DDGI-keyword passes (set per-view when a DDGIVolume resolved and
    // converged for the view's world — WorldRenderNode.cpp), so non-DDGI
    // variants never declare or expect b29-b32. Fallback texture is the 1x1
    // BLACK texture (same choice GTAO makes) — harmless even if a variant's
    // keyword and the feature's readiness briefly disagree, since
    // UploadVolumeData's own "enabled" flag (Params1.x) is the real gate
    // Includes/ibl.glsl checks before sampling it. The depth atlas (M3,
    // Chebyshev visibility) shares the same fallback/readiness logic as the
    // irradiance atlas — both are only ever valid or invalid together, since
    // EnsureGpuResources allocates them in the same pass.
    if (HasKeyword(passKeywords, MaterialKeyword::DDGI))
    {
        if (auto* ddgi = GetFeature<DDGIProbeFeature>(); ddgi && ddgi->IsInitialized())
        {
            const BufferHandle volumeData = ddgi->UploadVolumeData(m_Device);
            TextureHandle atlas = m_Textures->GetDefaultBlackTexture();
            TextureHandle depthAtlas = m_Textures->GetDefaultBlackTexture();
            SamplerHandle atlasSampler = ddgi->GetAtlasSampler();
            if (ddgi->GetIrradianceAtlas().IsValid() && atlasSampler.IsValid())
            {
                atlas = ddgi->GetIrradianceAtlas();
                depthAtlas = ddgi->GetDepthAtlas().IsValid() ? ddgi->GetDepthAtlas() : depthAtlas;
            }
            else
                atlasSampler = m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp);
            addTexture(HashStringId("ge_ddgiIrradianceAtlas"), atlas, atlasSampler);
            addTexture(HashStringId("ge_ddgiDepthAtlas"), depthAtlas, atlasSampler);
            // Reflected bindings resolve by the UBO's INSTANCE name (e.g.
            // "Env" for `uniform EnvDataUBO { ... } Env;` above), not its
            // block type name — MaterialBinder.cpp keys off b.Name, which
            // SPIRV-Reflect surfaces as the instance name. ddgi_probes.glsl
            // declares `uniform DDGIVolumeData { ... } DDGIVolume;` — the
            // reflected name is "DDGIVolume".
            add(HashStringId("DDGIVolume"), volumeData);
            // Unconditional: an unbound storage buffer is not a dimmer frame,
            // it is MaterialBinder dropping every draw in the world pass.
            // GetProbeStateBinding always yields a bindable buffer.
            const auto probeState = ddgi->GetProbeStateBinding();
            add(HashStringId("DDGIProbeStateRO"), probeState.Buffer, 0, probeState.Bytes);

            // M5 fine cascade (C1) — same fallback contract as C0 above: the
            // keyword's descriptor layout always grows these four bindings
            // (b33-36), so a valid-but-zero-weighted UBO/black-texture set
            // must always be supplied regardless of whether EnableFineCascade
            // is on, or a variant/feature-readiness mismatch would leave an
            // unbound descriptor.
            const BufferHandle volumeDataFine = ddgi->UploadVolumeDataFine(m_Device);
            TextureHandle atlasFine = m_Textures->GetDefaultBlackTexture();
            TextureHandle depthAtlasFine = m_Textures->GetDefaultBlackTexture();
            SamplerHandle atlasSamplerFine = ddgi->GetAtlasSampler();
            if (ddgi->GetIrradianceAtlasFine().IsValid() && atlasSamplerFine.IsValid())
            {
                atlasFine = ddgi->GetIrradianceAtlasFine();
                depthAtlasFine = ddgi->GetDepthAtlasFine().IsValid() ? ddgi->GetDepthAtlasFine() : depthAtlasFine;
            }
            else
                atlasSamplerFine = m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp);
            addTexture(HashStringId("ge_ddgiIrradianceAtlasFine"), atlasFine, atlasSamplerFine);
            // Same instance-vs-block-name fix as C0 above — ddgi_probes.glsl
            // declares `uniform DDGIVolumeDataFine { ... } DDGIVolumeFine;`.
            add(HashStringId("DDGIVolumeFine"), volumeDataFine);
            addTexture(HashStringId("ge_ddgiDepthAtlasFine"), depthAtlasFine, atlasSamplerFine);
            // C1's grid is only allocated while EnableFineCascade is on, but
            // b36 is declared either way — same unconditional bind as C0.
            const auto probeStateFine = ddgi->GetProbeStateBindingFine();
            add(HashStringId("DDGIProbeStateFineRO"), probeStateFine.Buffer, 0, probeStateFine.Bytes);

            // Reflection lobes (C0 at b37/b38, C1 at b39/b40) — same fallback
            // contract: the keyword's descriptor layout always declares all
            // four, so a variant/feature-readiness mismatch must still see a
            // bound (black) texture. EnableGlossy's real gate is each
            // cascade UBO's Params1.z.
            const SamplerHandle reflectionFallbackSampler =
                m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp);
            const SamplerHandle ddgiAtlasSampler = ddgi->GetAtlasSampler();

            TextureHandle roughAtlas = m_Textures->GetDefaultBlackTexture();
            SamplerHandle roughAtlasSampler = ddgiAtlasSampler;
            if (ddgi->GetRoughAtlas().IsValid() && ddgiAtlasSampler.IsValid())
                roughAtlas = ddgi->GetRoughAtlas();
            else
                roughAtlasSampler = reflectionFallbackSampler;
            addTexture(HashStringId("ge_ddgiRoughAtlas"), roughAtlas, roughAtlasSampler);

            TextureHandle glossyAtlas = m_Textures->GetDefaultBlackTexture();
            SamplerHandle glossyAtlasSampler = ddgiAtlasSampler;
            if (ddgi->GetGlossyAtlas().IsValid() && ddgiAtlasSampler.IsValid())
                glossyAtlas = ddgi->GetGlossyAtlas();
            else
                glossyAtlasSampler = reflectionFallbackSampler;
            addTexture(HashStringId("ge_ddgiGlossyAtlas"), glossyAtlas, glossyAtlasSampler);

            TextureHandle roughAtlasFine = m_Textures->GetDefaultBlackTexture();
            SamplerHandle roughAtlasFineSampler = ddgiAtlasSampler;
            if (ddgi->GetRoughAtlasFine().IsValid() && ddgiAtlasSampler.IsValid())
                roughAtlasFine = ddgi->GetRoughAtlasFine();
            else
                roughAtlasFineSampler = reflectionFallbackSampler;
            addTexture(HashStringId("ge_ddgiRoughAtlasFine"), roughAtlasFine, roughAtlasFineSampler);

            TextureHandle glossyAtlasFine = m_Textures->GetDefaultBlackTexture();
            SamplerHandle glossyAtlasFineSampler = ddgiAtlasSampler;
            if (ddgi->GetGlossyAtlasFine().IsValid() && ddgiAtlasSampler.IsValid())
                glossyAtlasFine = ddgi->GetGlossyAtlasFine();
            else
                glossyAtlasFineSampler = reflectionFallbackSampler;
            addTexture(HashStringId("ge_ddgiGlossyAtlasFine"), glossyAtlasFine, glossyAtlasFineSampler);

            // Scaled glossy-resolve outputs (b41/b42, Includes/ibl.glsl).
            // Same fallback contract as the atlases above: the DDGI keyword's
            // layout always declares both, so a pass without resolve targets
            // for this view (or GlossyResolveScale == Full, where the pass is
            // never declared) still sees bound black textures — and the C0
            // UBO's uParams2.w flag, not the binding, decides whether the
            // shader reads them.
            TextureHandle resolveRough = m_Textures->GetDefaultBlackTexture();
            TextureHandle resolveGlossy = m_Textures->GetDefaultBlackTexture();
            TextureHandle resolveIrradiance = m_Textures->GetDefaultBlackTexture();
            if (targets && targets->DDGIResolveRough.IsValid() &&
                targets->DDGIResolveGlossy.IsValid() &&
                targets->DDGIResolveIrradiance.IsValid())
            {
                const TextureHandle roughPhys = frame.PhysicalTexture(targets->DDGIResolveRough);
                const TextureHandle glossyPhys = frame.PhysicalTexture(targets->DDGIResolveGlossy);
                const TextureHandle irrPhys =
                    frame.PhysicalTexture(targets->DDGIResolveIrradiance);
                if (roughPhys.IsValid() && glossyPhys.IsValid() && irrPhys.IsValid())
                {
                    resolveRough = roughPhys;
                    resolveGlossy = glossyPhys;
                    resolveIrradiance = irrPhys;
                }
            }
            addTexture(HashStringId("ge_ddgiResolveRough"), resolveRough,
                       reflectionFallbackSampler);
            addTexture(HashStringId("ge_ddgiResolveGlossy"), resolveGlossy,
                       reflectionFallbackSampler);
            addTexture(HashStringId("ge_ddgiResolveIrradiance"), resolveIrradiance,
                       reflectionFallbackSampler);
            // Compat spellings: the WebGPU profile declares these as separate texture2D
            // images over the shared ge_iblSampler (ibl.glsl), so their reflected names carry
            // the Tex suffix. Both spellings are registered; the binder writes only the one the
            // active variant declares, exactly as the IBL trio does above.
            addTexture(HashStringId("ge_ddgiResolveRoughTex"), resolveRough,
                       reflectionFallbackSampler);
            addTexture(HashStringId("ge_ddgiResolveGlossyTex"), resolveGlossy,
                       reflectionFallbackSampler);
            addTexture(HashStringId("ge_ddgiResolveIrradianceTex"), resolveIrradiance,
                       reflectionFallbackSampler);
        }
    }

    return resources;
}

bool RenderServices::HasTransmissionInView(ViewId viewId)
{
    const uint64_t frame = m_Device ? m_Device->GetFrameIndex() : 0;
    if (const auto* pv = m_ViewRegistry.FindPerView(viewId); pv && pv->TransmissionMemo.first == frame)
        return pv->TransmissionMemo.second;
    const auto keys = m_WorldDrawBuilder.GetBatchKeys(viewId);
    const bool result =
        std::any_of(keys.begin(), keys.end(),
                    [](const WorldDrawBuilder::BatchKey& key)
                    {
                        return key.material &&
                               Rendering::HasKeyword(key.material->GetVariantKey().materialKeywords,
                                                     Rendering::MaterialKeyword::Transmission);
                    });
    m_ViewRegistry.PerView(viewId).TransmissionMemo = {frame, result};
    return result;
}

bool RenderServices::HasTransmissiveCasterInView(ViewId viewId) const
{
    return m_WorldDrawBuilder.HasTransmissiveCasterSubmissions(viewId);
}

SceneColorGrab& RenderServices::GetOrCreateTransmissionSceneGrab()
{
    if (!m_TransmissionSceneGrab)
        m_TransmissionSceneGrab =
            std::make_unique<SceneColorGrab>("View.EffectiveColor", "Transmission_SceneGrab");
    return *m_TransmissionSceneGrab;
}

class RenderServices::WorldPassPreparation
{
  public:
    WorldPassPreparation(RenderServices& services, WorldPassData& data,
                         RenderGraph::RGPrepareContext& context, Rendering::ViewId viewId,
                         Rendering::MaterialKeyword keywords, ReliefDepthSource reliefDepthSource,
                         WorldPassDrawScope scope, Rendering::GPUDrawStreamBuilder::SlicePhase phase,
                         uint8_t sliceCascadeIndex, RenderGraph::RGTexture prepassDepth,
                         Rendering::SamplerHandle prepassDepthSampler)
        : m_Services(services), m_Data(&data), m_Context(context), m_ViewId(viewId),
          m_PassKeywords(keywords), m_ReliefDepthSource(reliefDepthSource), m_Scope(scope),
          m_Phase(phase), m_SliceCascadeIndex(sliceCascadeIndex), m_PrepassDepth(prepassDepth),
          m_PrepassDepthSampler(prepassDepthSampler)
    {
    }

    bool Prepare()
    {
        m_Data->Draws.clear();
        m_Data->PushConstants.clear();
        m_Data->Inputs.PassResources.Textures.resize(m_Data->BaseTextureCount);
        BindVolumetricFogVolume(m_Context, m_Data->VolumetricFog, m_Data->FogSampler,
                                 m_Data->Inputs.PassResources);
        m_Data->HasAnyDraws = false;
        m_Data->EntityBatchCount = 0;
        m_Data->Consolidated = false;
        m_Data->LiveGroupCount = 0;
        m_EntityKeys = m_Services.m_WorldDrawBuilder.GetBatchKeys(m_ViewId);
        LogMeshPassDiagnostic(
            "color", "pass-enter", m_ViewId,
            m_EntityKeys.size(), m_Data->Commands.size(),
            m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
        // Contributors can draw even when the entity batch list is empty.
        m_HasEntityDraws = !m_EntityKeys.empty();
        m_Data->HasAnyDraws = m_HasEntityDraws || !m_Data->Commands.empty();
        if (!m_Data->HasAnyDraws)
        {
            LogMeshPassDiagnostic(
                "color", "pass-no-draws", m_ViewId,
                m_EntityKeys.size(), m_Data->Commands.size(),
                m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
            return true;
        }

        m_Scene = m_Services.GetGPUScene();

        m_InstanceAddress = 0;
        if (m_Services.m_Device && m_Scene && m_Scene->GetInstanceBuffer().IsValid())
            m_InstanceAddress =
                m_Services.m_Device->GetBufferDeviceAddress(m_Scene->GetInstanceBuffer());

        if (m_PrepassDepth.IsValid())
        {
            if (const TextureHandle depth = m_Context.GetTexture(m_PrepassDepth); depth.IsValid())
                m_Data->Inputs.PassResources.Textures.push_back(
                    {HashStringId("ge_prepassDepth"), depth, m_PrepassDepthSampler});
        }
        m_UploadedCompatListBytes =
            UploadedCompatInstanceListBytes(m_Data->Inputs.PassResources);
        m_Data->EntityBatchCount = m_EntityKeys.size();
        m_Data->Consolidated = !m_Services.MeshPoolGroupSpanForDraws().empty();
        m_Data->LiveGroupCount = m_Services.m_MeshPoolGroups.LiveGroupCount();
        m_ScatterReady = m_Services.m_DrawStreamBuilder && m_Services.m_DrawStreamBuilder->IntrospectScatter().PipelineReady;

        DrainForwardOpaqueFirst([this](bool blendPhase)
                                { PrepareEntities(blendPhase); },
                                [this](bool blendPhase)
                                { PrepareContributors(blendPhase); });

        // Vector growth is finished: install stable command/PC addresses once.
        for (auto& draw : m_Data->Draws)
        {
            draw.Command.Bindings.PushConstants = draw.PushConstantSize == 0
                                                      ? std::span<const std::byte>{}
                                                      : std::span<const std::byte>(m_Data->PushConstants.data() + draw.PushConstantOffset,
                                                                                   draw.PushConstantSize);
            draw.Prepared.Source = &draw.Command;
        }
        return true;
    }

  private:
    void Append(const DrawCommand& command, const PreparedForwardDraw& prepared,
                bool entity = false, uint32_t materialIndex = 0, uint32_t meshIndex = 0,
                uint32_t maxDrawCount = 0, size_t rangeCount = 0)
    {
        const auto bytes = command.Bindings.PushConstants;
        const size_t offset = m_Data->PushConstants.size();
        m_Data->PushConstants.insert(m_Data->PushConstants.end(), bytes.begin(), bytes.end());
        m_Data->Draws.push_back({command, prepared, offset, bytes.size(), entity,
                                 materialIndex, meshIndex, maxDrawCount, rangeCount});
    }

    void PrepareEntityBatch(const WorldDrawBuilder::BatchKey& key, const DrawStreamLookupKey& lookup,
                            const CpuDrawStreamBuilder::InstanceList* orderedRun = nullptr)
    {
        ComposedPCInstanced instanceConstants{};

        const Material* material = key.material;
        if (!material)
        {
            LogMeshDrawSkipDiagnostic(
                "color", "material-null", m_ViewId,
                key.materialIndex, key.meshIndex,
                m_ScatterReady,
                false, 0ull, m_InstanceAddress,
                m_Services.m_Device ? m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
            return;
        }
        if (IsOrderDependentBlendMaterial(*material) && m_Services.IsSortedTransparentActive(m_ViewId))
        {
            return;
        }
        if (m_Scope != WorldPassDrawScope::All)
        {
            const bool isTransmissive = Rendering::HasKeyword(
                material->GetVariantKey().materialKeywords, Rendering::MaterialKeyword::Transmission);
            if (m_Scope == WorldPassDrawScope::OpaqueOnly && isTransmissive)
            {
                LogMeshDrawSkipDiagnostic(
                    "color", "scope-opaque-filter", m_ViewId,
                    key.materialIndex, key.meshIndex,
                    m_ScatterReady,
                    false, 0ull, m_InstanceAddress,
                    m_Services.m_Device ? m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress : false,
                    m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
                return;
            }
            if (m_Scope == WorldPassDrawScope::TransmissiveOnly && !isTransmissive)
            {
                LogMeshDrawSkipDiagnostic(
                    "color", "scope-transmissive-filter", m_ViewId,
                    key.materialIndex, key.meshIndex,
                    m_ScatterReady,
                    false, 0ull, m_InstanceAddress,
                    m_Services.m_Device ? m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress : false,
                    m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
                return;
            }
        }
        const auto* entry = m_Services.GetMeshGPURegistry().Find(key.mesh);
        if (!entry)
        {
            LogMeshDrawSkipDiagnostic(
                "color", "mesh-entry-missing", m_ViewId,
                key.materialIndex, key.meshIndex,
                m_ScatterReady,
                false, 0ull, m_InstanceAddress,
                m_Services.m_Device ? m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
            return;
        }
        Rendering::MeshGPUEntryBindings entryBindings{};
        const bool drawable = m_Services.GetMeshGPURegistry().TryGetDrawableBindings(*entry, entryBindings);
        if (!drawable || entry->indexCount == 0)
        {
            LogMeshDrawSkipDiagnostic(
                "color", !drawable ? kMeshNotDrawableReason : "index-count-zero",
                m_ViewId, key.materialIndex, key.meshIndex,
                m_ScatterReady,
                false, 0ull, m_InstanceAddress,
                m_Services.m_Device ? m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
            return;
        }

        const Rendering::VertexAttributeFlags effectiveFlags =
            BoundStreamVertexFlags(*entry, entryBindings, material->IgnoresVertexColor());

        const Rendering::MaterialKeyword entityKeywords =
            m_PassKeywords | Rendering::MaterialKeyword::Instanced | WorldPassReliefDepthKeywords(material->GetVariantKey().materialKeywords, material->GetAlphaMode(), m_ReliefDepthSource);

        if (m_Services.m_Profile.IsCompat())
        {
            const CpuDrawStreamBuilder::InstanceList instances =
                orderedRun ? *orderedRun : m_Services.m_CpuDrawStream.GetInstances(m_ViewId, key.materialIndex, key.meshIndex, CpuDrawStreamBuilder::InstanceSet::Camera);
            if (instances.Count == 0u)
                return;
            assert(m_UploadedCompatListBytes == m_Services.m_CpuDrawStream.GetIndexList(m_ViewId).size_bytes() &&
                   "the view's instance list was rebuilt after its first pass declared");

            auto [pipeline, variantEntry] = m_Services.m_MaterialSystem.Variants().PrepareColorVariant(
                *material, effectiveFlags, entry->topology, entityKeywords, m_Services.m_FrontFace, m_Context);
            if (!pipeline.IsValid())
            {
                LogMeshDrawSkipDiagnostic(
                    "color", "color-pipeline-invalid", m_ViewId,
                    key.materialIndex, key.meshIndex, false, false, 0ull,
                    m_InstanceAddress, false, 0u);
                return;
            }

            // firstInstance stays 0: it does not reach gl_InstanceIndex
            // through SPIRV-Cross/Metal, so the list offset is the push
            // constant on every compat backend.
            const uint32_t compatListFirst = instances.First;
            Engine::Renderer::DrawBindings compatBindings{};
            compatBindings.PushConstants = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(&compatListFirst),
                sizeof(compatListFirst));

            const auto preparedBindings = m_Services.Materials().Binder().PrepareMaterialForDraw(
                m_Data->Inputs, *material, pipeline,
                variantEntry ? variantEntry->VariantMeta.get() : nullptr, compatBindings, /*pipelineSetCount=*/0,
                variantEntry ? std::span<const Rendering::DescriptorSetLayoutId>(variantEntry->SetLayouts)
                             : std::span<const Rendering::DescriptorSetLayoutId>{});
            if (!preparedBindings.Pipeline.IsValid())
            {
                LogMeshDrawSkipDiagnostic(
                    "color", "material-bind-invalid", m_ViewId,
                    key.materialIndex, key.meshIndex, false, false, 0ull,
                    m_InstanceAddress, false, 0u);
                return;
            }

            DrawCommand command{};
            command.Bindings = compatBindings;
            PreparedForwardDraw prepared{nullptr, preparedBindings, entryBindings, {entry->indexCount, instances.Count, entry->firstIndex, 0u, static_cast<int32_t>(entry->vertexOffset)}, {}, material->GetGpuSceneMaterialIndex()};
            prepared.Geometry.indexType = entry->indexType;
            if (variantEntry)
                prepared.ServedVersion = variantEntry->MaterialVersion;
            Append(command, prepared, true, key.materialIndex, key.meshIndex, 1u, 0u);
            return;
        }

        if (!m_ScatterReady)
        {
            LogMeshDrawSkipDiagnostic(
                "color", "drawstream-pipeline-invalid", m_ViewId,
                key.materialIndex, key.meshIndex,
                false, false, 0ull, m_InstanceAddress,
                m_Services.m_Device ? m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.m_DrawStreamBuilder ? m_Services.m_DrawStreamBuilder->GetRangeCount() : 0u);
            return;
        }
        // The pass's own Color-table slice; the caller resolved both axes.
        // The representative material (key.material) binds the PSO.
        const Rendering::GPUDrawStreamBuilder::BatchDrawRange drawRange =
            m_Services.m_DrawStreamBuilder->FindBatchDrawRange(
                static_cast<uint32_t>(m_ViewId), m_SliceCascadeIndex,
                lookup.classKey, lookup.meshKey, m_Phase);
        if (!drawRange.IsValid() || !m_Services.m_Device)
        {
            LogMeshDrawSkipDiagnostic(
                "color", !m_Services.m_Device ? "device-null" : "draw-range-invalid", m_ViewId,
                key.materialIndex, key.meshIndex,
                true, drawRange.IsValid(), 0ull, m_InstanceAddress,
                m_Services.m_Device ? m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress : false,
                m_Services.m_DrawStreamBuilder->GetRangeCount());
            return;
        }
        // Shared indirection buffer: records carry arena-global
        // firstInstance, so one BDA serves every batch and slice.
        const uint64_t slotIndirectionBda =
            m_Services.m_DrawStreamBuilder->GetSharedIndirectionAddress();
        if (slotIndirectionBda == 0 || m_InstanceAddress == 0)
        {
            LogMeshDrawSkipDiagnostic(
                "color", slotIndirectionBda == 0 ? "indirection-bda-zero" : "instance-bda-zero",
                m_ViewId, key.materialIndex, key.meshIndex,
                true, true, slotIndirectionBda, m_InstanceAddress,
                m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.m_DrawStreamBuilder->GetRangeCount());
            return;
        }

        instanceConstants.indirectionAddr = slotIndirectionBda;
        instanceConstants.gpuInstancesAddr = m_InstanceAddress;
        std::span<const std::byte> constantBytes(
            reinterpret_cast<const std::byte*>(&instanceConstants), sizeof(instanceConstants));

        Engine::Renderer::DrawBindings drawBindings{};
        drawBindings.PushConstants = constantBytes;

        const WorldBatchPreparation batch{key, material, entry, entryBindings, effectiveFlags,
                                          entityKeywords, drawRange, drawBindings, slotIndirectionBda};
        const Rendering::FrontFace baseWinding = m_Services.m_FrontFace;
        for (const auto& drawSegment : drawRange.Segments())
            PrepareSegment(batch, drawSegment.mirrored ? Rendering::FlipWinding(baseWinding) : baseWinding,
                           drawSegment.crossfading, drawSegment.segment);
    }

    void PrepareSegment(const WorldBatchPreparation& batch, Rendering::FrontFace winding,
                        bool crossfading, const Rendering::GPUDrawStreamBuilder::BatchDrawSegment& segment)
    {
        const size_t commandOffset = segment.cmdByteOffset;
        const size_t countOffset = segment.countByteOffset;
        const uint32_t maxDrawCount = segment.maxDrawCount;
        const Rendering::MaterialKeyword segmentKeywords =
            crossfading ? (batch.Keywords | Rendering::MaterialKeyword::LodCrossfade)
                        : batch.Keywords;
        auto variant = m_Services.m_MaterialSystem.Variants().PrepareColorVariant(
            *batch.Material, batch.VertexFlags, batch.Mesh->topology, segmentKeywords, winding, m_Context);
        // A cold fade variant uses the head variant to preserve mesh coverage.
        if (!variant.first.IsValid() && crossfading)
        {
            variant = m_Services.m_MaterialSystem.Variants().PrepareColorVariant(
                *batch.Material, batch.VertexFlags, batch.Mesh->topology, batch.Keywords, winding,
                m_Context);
        }
        auto [pipeline, variantEntry] = variant;
        if (!pipeline.IsValid())
        {
            LogMeshDrawSkipDiagnostic(
                "color", "color-pipeline-invalid", m_ViewId,
                batch.Key.materialIndex, batch.Key.meshIndex, true,
                false, batch.IndirectionAddress, m_InstanceAddress,
                m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.m_DrawStreamBuilder->GetRangeCount());
            return;
        }
        const auto preparedBindings = m_Services.Materials().Binder().PrepareMaterialForDraw(
            m_Data->Inputs, *batch.Material, pipeline,
            variantEntry ? variantEntry->VariantMeta.get() : nullptr, batch.Bindings, /*pipelineSetCount=*/0,
            variantEntry ? std::span<const Rendering::DescriptorSetLayoutId>(variantEntry->SetLayouts)
                         : std::span<const Rendering::DescriptorSetLayoutId>{});
        if (!preparedBindings.Pipeline.IsValid())
        {
            LogMeshDrawSkipDiagnostic(
                "color", "material-bind-invalid", m_ViewId,
                batch.Key.materialIndex, batch.Key.meshIndex,
                true, true, batch.IndirectionAddress, m_InstanceAddress,
                m_Services.m_Device->GetCapabilities().supportsBufferDeviceAddress,
                m_Services.m_DrawStreamBuilder->GetRangeCount());
            return;
        }
        DrawCommand command{};
        command.Bindings = batch.Bindings;
        command.UseIndirect = true;
        command.IndirectCommandBuffer = batch.Range.recordBuffer;
        command.IndirectCountBuffer = batch.Range.countBuffer;
        command.IndirectMaxDrawCount = maxDrawCount;
        command.IndirectStride = 5u * sizeof(uint32_t);
        command.IndirectCommandOffset = commandOffset;
        command.IndirectCountOffset = countOffset;
        PreparedForwardDraw prepared{nullptr, preparedBindings, batch.Geometry, {}, {}, batch.Material->GetGpuSceneMaterialIndex()};
        prepared.Geometry.indexType = batch.Mesh->indexType;
        if (variantEntry)
            prepared.ServedVersion = variantEntry->MaterialVersion;
        Append(command, prepared, true, batch.Key.materialIndex, batch.Key.meshIndex,
               maxDrawCount, m_Services.m_DrawStreamBuilder->GetRangeCount());
    }

    void PrepareContributor(const DrawCommand& command)
    {
        // Main-world contributors require a material, unlike the separate forward API.
        if (!command.Material)
            return;
        if (auto prepared = PrepareForwardDrawCommand(command, 0, m_Services, m_Data->Inputs, m_Services.m_FrontFace, m_Context))
            Append(command, *prepared);
    }

    void PrepareEntities(bool blendPhase)
    {
        if (!m_HasEntityDraws)
            return;
        const std::span<const uint32_t> meshGroups = m_Services.MeshPoolGroupSpanForDraws();
        // Color-table slices key on the color class whatever their
        // cascade index: MaterialDependent keeps the depth-class remap
        // (a Shadow-table concern) out of the resolution.

        if (blendPhase && m_Services.m_Profile.IsCompat() && m_Scene)
        {
            struct OrderedInstance
            {
                const WorldDrawBuilder::BatchKey* Key;
                uint32_t IndexOffset;
                float Depth;
            };
            std::vector<OrderedInstance> ordered;
            const auto camera = Rendering::DeriveCameraData(m_Services.m_ViewRegistry.ResolveCameraData(m_ViewId),
                                                            m_Services.m_ViewRegistry.ResolveCameraRanges(m_ViewId));
            const auto& instances = m_Scene->GetInstances();
            for (const auto& key : m_EntityKeys)
            {
                if (!IsBlendedMaterial(key.material))
                    continue;
                const auto list = m_Services.m_CpuDrawStream.GetInstances(m_ViewId, key.materialIndex,
                                                                          key.meshIndex, CpuDrawStreamBuilder::InstanceSet::Camera);
                const auto indices = m_Services.m_CpuDrawStream.GetIndexList(m_ViewId);
                {
                    for (uint32_t instanceIndex = 0; instanceIndex < list.Count; ++instanceIndex)
                    {
                        const uint32_t instance = indices[list.First + instanceIndex];
                        if (instance >= instances.size())
                            continue;
                        const auto& center = instances[instance].boundingCenter;
                        const float depth = (center.x - camera.Position.x) * camera.Forward.x +
                                            (center.y - camera.Position.y) * camera.Forward.y +
                                            (center.z - camera.Position.z) * camera.Forward.z;
                        ordered.push_back({&key, list.First + instanceIndex, depth});
                    }
                }
            }
            std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
                             {
                    if (a.Depth != b.Depth) return a.Depth > b.Depth;
                    return a.IndexOffset < b.IndexOffset; });
            for (size_t instanceIndex = 0; instanceIndex < ordered.size();)
            {
                const auto& first = ordered[instanceIndex];
                CpuDrawStreamBuilder::InstanceList run{first.IndexOffset, 1};
                size_t next = instanceIndex + 1;
                // Coalesce only neighbours whose sorted order remains
                // identical to consecutive entries in the uploaded index list.
                while (next < ordered.size() && ordered[next].Key == first.Key &&
                       ordered[next].IndexOffset == run.First + run.Count)
                {
                    ++run.Count;
                    ++next;
                }
                PrepareEntityBatch(*first.Key, Lookup(*first.Key, meshGroups), &run);
                instanceIndex = next;
            }
            return;
        }
        if (meshGroups.empty())
        {
            for (const auto& key : m_EntityKeys)
                if (IsBlendedMaterial(key.material) == blendPhase)
                    PrepareEntityBatch(key, Lookup(key, meshGroups));
        }
        else
        {
            std::unordered_set<uint64_t> drawnGroups;
            drawnGroups.reserve(m_EntityKeys.size());
            for (const auto& key : m_EntityKeys)
            {
                if (IsBlendedMaterial(key.material) != blendPhase)
                    continue;
                const DrawStreamLookupKey lookup = Lookup(key, meshGroups);
                if (lookup.meshKey == Rendering::MeshPoolGroupPlan::kAbsentGroup)
                    continue; // dead mesh row — instances self-reject GPU-side
                const uint64_t drawKey =
                    (static_cast<uint64_t>(lookup.classKey) << 24) | lookup.meshKey;
                if (!drawnGroups.insert(drawKey).second)
                    continue;
                PrepareEntityBatch(key, lookup);
            }
        }
    }

    DrawStreamLookupKey Lookup(const WorldDrawBuilder::BatchKey& key,
                               std::span<const uint32_t> meshGroups) const
    {
        return ResolveDrawStreamLookupKey(
            key, m_SliceCascadeIndex,
            Rendering::MaterialDepthClass::MaterialDependent, meshGroups);
    }

    void PrepareContributors(bool blendPhase)
    {
        if (m_Scope == WorldPassDrawScope::TransmissiveOnly)
            return;
        for (const auto& command : m_Data->Commands)
            if (IsBlendedContributor(command) == blendPhase)
                PrepareContributor(command);
    }

    RenderServices& m_Services;
    WorldPassData* m_Data;
    RenderGraph::RGPrepareContext& m_Context;
    Rendering::ViewId m_ViewId;
    Rendering::MaterialKeyword m_PassKeywords;
    ReliefDepthSource m_ReliefDepthSource;
    WorldPassDrawScope m_Scope;
    Rendering::GPUDrawStreamBuilder::SlicePhase m_Phase;
    uint8_t m_SliceCascadeIndex;
    RenderGraph::RGTexture m_PrepassDepth;
    Rendering::SamplerHandle m_PrepassDepthSampler;
    std::span<const WorldDrawBuilder::BatchKey> m_EntityKeys;
    Rendering::GPUScene* m_Scene = nullptr;
    uint64_t m_InstanceAddress = 0;
    uint64_t m_UploadedCompatListBytes = 0;
    bool m_HasEntityDraws = false;
    bool m_ScatterReady = false;
};

RenderServices::WorldPassRG RenderServices::AddWorldPassForView(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    const WorldPassTargetsRG& targets, MaterialKeyword passKeywords, WorldPassDrawScope scope,
    uint8_t sliceCascadeIndex)
{
    return AddWorldPassImpl(frame, viewId, targets, passKeywords, scope,
                            Rendering::GPUDrawStreamBuilder::SlicePhase::A, sliceCascadeIndex);
}

RenderServices::WorldPassRG RenderServices::AddWorldColorRecoverPassForView(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    const WorldPassTargetsRG& targets, MaterialKeyword passKeywords, WorldPassDrawScope scope)
{
    return AddWorldPassImpl(frame, viewId, targets, passKeywords, scope,
                            Rendering::GPUDrawStreamBuilder::SlicePhase::B,
                            Rendering::GPUDrawStreamBuilder::kCascadeIndexNone);
}

RenderServices::WorldPassRG RenderServices::AddWorldPassImpl(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    const WorldPassTargetsRG& targets, MaterialKeyword passKeywords, WorldPassDrawScope scope,
    Rendering::GPUDrawStreamBuilder::SlicePhase phase, uint8_t sliceCascadeIndex)
{
    namespace RenderGraph = Rendering::RenderGraph;

    // Phase B is the additive HZB "recover" pass: it LOADS the phase-A colour +
    // depth (never clears), does not arm WorldDeclared, and consumes phase-B
    // scatter ranges. Everything else (resources, keywords, resolve, shadows,
    // IBL) is identical to phase A so recovered geometry shades the same.
    const bool isRecover = (phase == Rendering::GPUDrawStreamBuilder::SlicePhase::B);

    float bandStart = 0.0f, bandEnd = 0.0f;
    int distanceMode = 0;
    if (GetDepthPixelationBandRange(viewId, bandStart, bandEnd, distanceMode))
        passKeywords |= Rendering::MaterialKeyword::DepthPixelationBand;

    WorldPassRG result{};

    const ViewDesc* view = nullptr;
    for (const auto& v : m_ViewRegistry.GetViews())
    {
        if (v.id == viewId)
        {
            view = &v;
            break;
        }
    }
    if (!view)
        return result;

    // The old activation predicate, evaluated at declaration: not declared =
    // not compiled. (The renderLayerMask/external-request term is the
    // slice-4 spine's job — calling this function IS the request.)
    const bool hasTargets = targets.Color.IsValid() || targets.Depth.IsValid();
    // The recover pass never activates just to clear (it always loads) — it only
    // runs when there is phase-B geometry to draw.
    const bool anyClearRequested =
        !isRecover && (view->targets.clearColor || view->targets.clearDepth ||
                       view->targets.clearStencil);
    // Phase B only recovers GPU-cullable entity batches. Explicit forward
    // contributors (terrain, grass, ocean, fog) are not part of the HZB
    // scatter and were already drawn in phase A; draining them here draws the
    // entire contributor a second time.
    const bool hasForwardContributors = !isRecover && HasForwardCommands(viewId);
    if (!hasTargets ||
        (!anyClearRequested && m_WorldDrawBuilder.GetBatchKeys(viewId).empty() &&
         !hasForwardContributors))
        return result;

    std::string passName = "RenderEntities[";
    if (view->debugName && view->debugName[0] != '\0')
        passName += view->debugName;
    else
        passName += "View";
    passName += "#";
    passName += std::to_string(static_cast<uint32_t>(viewId));
    passName += "]";
    if (sliceCascadeIndex != Rendering::GPUDrawStreamBuilder::kCascadeIndexNone)
        passName += ".Slice" + std::to_string(static_cast<uint32_t>(sliceCascadeIndex));
    if (scope == WorldPassDrawScope::TransmissiveOnly)
        passName = "Transmissive" + passName;
    if (isRecover)
        passName += "B";

    // Shadow array: CONSUME-only — the SAME resource id the cascade arm
    // imported and attached earlier this frame (contract order), so the Read
    // below chains from the depth writes. The world never CREATES the array:
    // ImportShadowMapArrayRG allocates ~Resolution²×cascades of D32 from the
    // pool, and a pipeline with no cascade producer (shadowless blueprint
    // with shadow-capable materials, keyword-less thumbnails) must not pay
    // that for an array nothing ever writes — with no producer this frame
    // the world binds the dummy, exactly like a keyword-less view.
    const bool wantsShadows =
        Rendering::HasKeyword(passKeywords, Rendering::MaterialKeyword::Shadows);
    auto& vfr = ViewFrameRGFor(frame, viewId);
    const RenderGraph::RGTexture shadowArr =
        (wantsShadows && vfr.ShadowMapArray.IsValid()) ? vfr.ShadowMapArray : RenderGraph::RGTexture{};

    // MSM moments: dedup FALLBACK import (feature-owned physical). On MSM4
    // frames the producer node (ShadowMapNode) owns the import and
    // handle-dedup lands both sites on one id (RGFrame FindImport) — this arm
    // only creates the resource when the producer declared nothing this frame
    // (PCSS frames: nothing writes, so declared ShaderResource is truthful).
    // On producer frames the world's all-layer Sampled read below is the edge
    // that homogenizes written layers back to ShaderReadOnly.
    if (auto* feature = GetFeature<ShadowMapRenderFeature>();
        wantsShadows && feature && feature->IsInitialized() && !vfr.MsmMoments.IsValid())
    {
        if (auto momentsTex = feature->GetMsmMomentsTexture(viewId); momentsTex.IsValid())
        {
            const auto& cfg = feature->GetConfig();
            const std::string momentsName =
                "MsmMoments.View" + std::to_string(static_cast<uint32_t>(viewId));
            vfr.MsmMoments = frame.ImportExternalTexture(
                momentsName.c_str(), momentsTex, Rendering::ResourceState::ShaderResource,
                Rendering::TextureFormat::R16G16B16A16_UNORM, 1, cfg.NumCascades);
        }
    }

    // Phase A arms the misorder tripwire (producers after this point derive
    // WAR). The recover pass LEGITIMATELY follows the phase-A raster, so it must
    // not arm it (and the phase-A pass already published the blackboard).
    if (!isRecover)
        vfr.WorldDeclared = true;

    // Depth-clear suppression + read-only depth: the RenderGraph prepass is declared
    // only when it runs and always clears, so the old prepassWillExecute
    // dance collapses to the per-frame record. Texture IDENTITY, not a bool:
    // a prepass that ran on an override depth didn't write THIS depth, and
    // suppressing the clear would depth-test against stale content.
    const bool hasDepthPrepass = vfr.DepthPrepassDepth.IsValid() && targets.Depth.IsValid() &&
                                 vfr.DepthPrepassDepth.Id == targets.Depth.Id;
    // Recover always loads the phase-A depth (the phase-B depth recover already
    // wrote newly-revealed fragments into it) — never clears.
    //
    // The transmissive pass composites over an already-rasterized opaque scene and attaches that
    // scene's depth read-only: clearing it would both discard depth later passes still read and
    // leave the glass testing against a cleared buffer. Independent of the prepass, whose absence
    // would otherwise let the view's clearDepth through on a pass that only ever loads.
    const bool suppressDepthClear =
        hasDepthPrepass || isRecover || scope == WorldPassDrawScope::TransmissiveOnly;
    const bool needsClear = view->targets.clearColor ||
                            (view->targets.clearDepth && !suppressDepthClear) ||
                            view->targets.clearStencil;
    const auto entityBatchKeys = m_WorldDrawBuilder.GetBatchKeys(viewId);

    // The prepass wrote this view's opaque depth, Mask coverage included (its alpha-tested heads run
    // the colour pass's own cutoff), and the depth of every forward draw whose producer emitted its
    // head there (ForwardDrawDepth::Prepass, or PrepassNonOccluding through the non-occluding prepass
    // after DepthResolve), so the world pass only tests against it. A forward
    // draw that writes its own depth (ForwardDrawDepth::ColourPass) keeps it writable.
    const ViewRegistry::PerViewResources* forwardView = m_ViewRegistry.FindPerView(viewId);
    bool depthReadOnly =
        hasDepthPrepass && !(forwardView && forwardView->ForwardDrawsWritingDepth != 0u);
    // A Blend material that authors zWrite writes its own depth in this pass whenever blended
    // entities draw here (compatibility profile, drain fallback, order-independent blends). A
    // read-only attachment drops those writes, so later depth readers (a DepthResolve after
    // SortedTransparent) would see the backdrop behind the sprite instead.
    if (depthReadOnly &&
        std::any_of(entityBatchKeys.begin(), entityBatchKeys.end(),
                    [](const WorldDrawBuilder::BatchKey& key)
                    {
                        return key.material && key.material->GetAlphaMode() == MaterialAlphaMode::Blend &&
                               key.material->GetDepthWriteOverride().value_or(false);
                    }))
        depthReadOnly = false;
    // The transmissive pass never writes depth: glass composites over the lit opaque scene
    // and only depth-tests (ReadOnly) against the opaque depth — single-layer, order-independent.
    if (scope == WorldPassDrawScope::TransmissiveOnly)
        depthReadOnly = true;

    // A parallax material's colour draw reads the relief depth the prepass wrote, from the depth it
    // attaches read-only (ge_prepassDepth), and rebuilds the hit instead of marching again. The read is
    // declared only when a parallax material is in view.
    const ReliefDepthSource reliefDepthSource = WorldPassReliefDepthSource(
        hasDepthPrepass, depthReadOnly && scope != WorldPassDrawScope::TransmissiveOnly,
        targets.Depth.IsValid() ? frame.Graph().ResourceDesc(targets.Depth.Id).SampleCount : 1u);
    const bool readsReliefDepth =
        (reliefDepthSource == ReliefDepthSource::PrepassReadable ||
         reliefDepthSource == ReliefDepthSource::PrepassReadableMultisample) &&
        std::any_of(entityBatchKeys.begin(), entityBatchKeys.end(),
                    [](const WorldDrawBuilder::BatchKey& key)
                    { return key.material && WritesReliefDepth(key.material->GetVariantKey().materialKeywords); });

    // Recover loads the phase-A colour (additive) — treat it as already
    // initialized so the attach below takes the Load branch, never Clear.
    const bool colorAlreadyInit =
        isRecover || m_ViewRegistry.IsViewColorInitialized(viewId) || m_ViewRegistry.IsViewSkyBackdropScheduled(viewId);

    // MSAA resolve three-way, decided from declared descs (old: TryGetTextureDesc).
    bool canResolveColor = false;
    bool collapseToResolve = false;
    if (targets.Color.IsValid() && targets.Resolve.IsValid() &&
        targets.Resolve.Id != targets.Color.Id)
    {
        const auto& colorDesc = frame.Graph().ResourceDesc(targets.Color.Id);
        const auto& resolveDesc = frame.Graph().ResourceDesc(targets.Resolve.Id);
        if (resolveDesc.SampleCount == 1u)
        {
            if (colorDesc.SampleCount > 1u)
            {
                // Resolve requires matching formats (VUID 06865); a mismatch
                // means the resolve target is wrong for this color — render
                // to the MSAA color only rather than emit an invalid resolve.
                if (colorDesc.Format == resolveDesc.Format)
                    canResolveColor = true;
                else
                {
                    static std::atomic<int> sFmtWarnBudget{8};
                    if (sFmtWarnBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
                        Logger::Log::Warning(
                            "RenderServices WorldView.Color (RenderGraph): skipping MSAA resolve -- "
                            "color fmt {} vs resolve fmt {}",
                            colorDesc.Format, resolveDesc.Format);
                }
            }
            else if (targets.ResolveFromPipeline)
            {
                // MSAA off: render straight into the pipeline's resolve target
                // so the post-FX chain never samples an uninitialized texture.
                collapseToResolve = true;
            }
        }
    }
    result.EffectiveColor = (canResolveColor || collapseToResolve) ? targets.Resolve
                                                                   : targets.Color;

    // Viewport snapshot at declaration (old: exec-time GetTextureSize).
    uint32_t viewportW = 0;
    uint32_t viewportH = 0;
    {
        const RenderGraph::RGTexture vpTex = targets.Resolve.IsValid() ? targets.Resolve : targets.Color;
        if (vpTex.IsValid())
        {
            const auto& d = frame.Graph().ResourceDesc(vpTex.Id);
            viewportW = d.Width;
            viewportH = d.Height;
        }
    }
    // Publish the view's render-target height for the SSE-budget LOD scale
    // (read at next frame's scatter registration; see PerViewResources).
    if (viewportH != 0u)
        m_ViewRegistry.PerView(viewId).WorldViewportHeight = viewportH;
    // Publish the color attachment's sample count the same way (grass reads it
    // at next frame's forward emit to pick alpha-to-coverage vs screen-door
    // dither; one frame stale across an MSAA toggle, self-heals).
    if (targets.Color.IsValid())
        m_ViewRegistry.PerView(viewId).WorldColorSampleCount =
            frame.Graph().ResourceDesc(targets.Color.Id).SampleCount;

    // Camera UBO: upload-ring alloc written NOW. Clear-only frames still get
    // valid camera data — the old arm wrote before its draw guard for the
    // same reason. Raster domain: TAA views rasterize with the jittered
    // projection (no-op when TAA is off).
    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
        *cam.Ptr = m_ViewRegistry.ResolveJitteredCameraData(viewId, frame.FrameIndex(),
                                                            viewportW, viewportH);

    const ResolvedShadowSettings& worldShadowSettings = GetWorldShadowSettings(view->worldId);
    // The ray-traced mask (DirectionalShadowMode::RayTraced) replaces the directional shadow
    // factor's source for opaque world passes. It reads single-sample prepass-written depth only:
    // without a prepass the depth is unwritten until the world pass itself rasterizes, and the
    // mask would trace clears. It unprojects with the same jittered camera the prepass rasterized
    // with (exact reconstruction under TAA; no-op when TAA is off).
    if (m_RTShadowMask)
        passKeywords = m_RTShadowMask->ContributeWorldPass(
            *this, frame, viewId, targets.DepthResolved.IsValid() ? targets.DepthResolved : targets.Depth,
            cam.Valid() ? cam.Ptr : nullptr, !isRecover && vfr.DepthPrepassDepth.IsValid(),
            scope == WorldPassDrawScope::TransmissiveOnly, vfr.RTShadowMask, passKeywords);

    const ScreenSpaceShadowView contactView{
        static_cast<uint32_t>(viewId), view->worldId,
        targets.OccluderDepthResolved.IsValid() ? targets.OccluderDepthResolved
        : targets.DepthResolved.IsValid()       ? targets.DepthResolved
                                                : targets.Depth,
        cam.Valid() ? cam.Ptr : nullptr, worldShadowSettings,
        !isRecover && vfr.DepthPrepassDepth.IsValid(),
        scope == WorldPassDrawScope::TransmissiveOnly, vfr.RTShadowMask.IsValid()};
    passKeywords = ContributeScreenSpaceShadows(
        *this, frame, contactView, m_ScreenSpaceShadows, vfr.ScreenSpaceShadowMask, passKeywords);

    // MaterialKeyword::LodCrossfade is deliberately NOT set here. It is a
    // per-SEGMENT property (BatchDrawRange::Segments) selected by the entity
    // recorders for tail draws only, because the dither's `discard` forfeits
    // early-Z on every draw it is compiled into and only tail records carry a
    // fade code. Setting it view-wide would put that discard into every head
    // draw and every forward contributor — terrain, grass and ocean take the
    // full GetWorldPassKeywords set and have no mesh LOD chain to fade. The
    // depth prepass reaches the same conclusion independently: it narrows this
    // set to the Instanced bit and then adds LodCrossfade back per tail segment
    // (DepthDrawRecorder), so its dither and this pass's are the same test on
    // the same records.

    // Keep the recorded keywords current in both arms: the depth-pass keyword
    // narrowing reads them, and so does the blueprint-raced fallback in
    // ResolveWorldPassKeywordsForPrewarm. External callers (thumbnails) pass
    // None, which leaves the recorded value unchanged.
    if (passKeywords != Rendering::MaterialKeyword::None)
        m_ViewRegistry.PerView(viewId).WorldPassKeywords = passKeywords;

    // Coverage census (GE_PROP_CENSUS; throttled). Declaration-time: extraction
    // is complete and this pass's scatter snapshot was captured from the same
    // CPU instance state. Skipped for the recover/transmissive re-entries so
    // each view logs once.
    if (PropCensusEnabled() && !isRecover && scope == WorldPassDrawScope::All &&
        cam.Valid() && m_Device != nullptr && (m_Device->GetFrameIndex() % 120u) == 0u)
    {
        if (const Rendering::GPUScene* censusScene = GetGPUScene())
            LogPropCoverageCensus(*censusScene, *cam.Ptr, static_cast<uint32_t>(viewId),
                                  viewportW, viewportH, WorldKeyForView(viewId));
    }

    const ViewLetterbox letterbox = m_ViewRegistry.GetViewLetterbox(viewId);
    // Planar-reflection winding fix: a mirror-matrix view flips triangle winding,
    // so flip the viewport Y to re-invert it (front face stays correct). See
    // SetViewWorldPassFlipY. The sampler flips V to compensate the image flip.
    const bool flipViewportY = m_ViewRegistry.GetViewWorldPassFlipY(viewId);

    ResolvedPassResources resources =
        BuildPassResourcesRG(frame, viewId, passKeywords, &targets, cam.Buffer, cam.Offset);
    const DDGIProbeFeature::GatherReadsRG ddgiReads =
        ImportDdgiGatherReads(frame, GetFeature<DDGIProbeFeature>(), passKeywords);
    // ge_prepassDepth is the depth attachment itself, which a multisampled view allocates per frame:
    // resolved to its physical texture at record time, below.
    const RenderGraph::RGTexture prepassDepth = readsReliefDepth ? targets.Depth : RenderGraph::RGTexture{};
    const SamplerHandle prepassDepthSampler = m_Textures->GetSampler(Rendering::SamplerPreset::PointClamp);

    // Snapshot the view's forward-contributor commands AT DECLARATION: the
    // stream is per view and REPLACED by every emit, so a second pass declared
    // on the same view this frame (a probe's next cube face) would otherwise
    // hand this pass the later face's commands at exec.
    // Explicit contributors do not participate in the phase-B HZB recovery
    // stream — they were already drawn in phase A, so the recover pass takes
    // an empty list rather than drawing each contributor twice.
    std::span<const DrawCommand> forwardSnapshot;
    if (phase != Rendering::GPUDrawStreamBuilder::SlicePhase::B)
    {
        const std::span<const DrawCommand> emitted = GetForwardCommands(viewId);
        forwardSnapshot = CaptureForwardCommands(frame, emitted);
    }

    auto* data = frame.AllocatePassData<WorldPassData>();
    data->Inputs.View = viewId;
    data->Inputs.PassKeywords = passKeywords;
    data->Inputs.PassInstanceIndex = WorldPassInstanceIndex(sliceCascadeIndex, phase);
    data->Inputs.PassResources = std::move(resources);
    data->Commands = forwardSnapshot;
    data->BaseTextureCount = data->Inputs.PassResources.Textures.size();
    data->VolumetricFog = HasKeyword(passKeywords, MaterialKeyword::VolumetricFog)
                              ? targets.VolumetricFog : RenderGraph::RGTexture{};
    data->FogSampler = m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp);

    result.Pass = frame.AddPass(
        passName.c_str(), Rendering::PassPhase::kWorldRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            // A2.4-P0-R: the world/forward+ exec is self-contained — it sets its
            // own viewport/scissor and binds per draw (CONC-F13), so its interior
            // may record into a worker secondary under GE_PARALLEL_RECORD.
            p.RecordInSecondary();
            // Slice-3 contract reads. SkinPaletteAtlas is the compute→VS
            // barrier (the VS binds it descriptor-direct, so only this
            // declared read orders skinning). DrawStreamOrdering is the
            // bucketer's ordering proxy: the Read must be recorded AFTER its
            // Write or the edge derives WAR and draws consume frame N−1 slots.
            if (m_FrameRG.For.IsFor(frame) && m_FrameRG.SkinPaletteAtlas.IsValid())
                p.Read(m_FrameRG.SkinPaletteAtlas);
            else if (m_GPUAnimDataStore.GetInstanceCount() != 0)
                Logger::Log::Error(
                    "[RenderGraph] AddWorldPassForView(view {}): skinned instances pending but no "
                    "SkinPaletteAtlas value is published — declare "
                    "ScheduleGpuSkinningAndRetarget(frame) BEFORE the world pass",
                    viewId);
            if (m_FrameRG.For.IsFor(frame) && m_FrameRG.DrawStreamOrdering.IsValid())
                p.Read(m_FrameRG.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
            else if (m_DrawStreamBuilder && m_DrawStreamBuilder->GetPendingSliceCount() != 0)
                Logger::Log::Error(
                    "[RenderGraph] AddWorldPassForView(view {}): buckets are registered but no "
                    "DrawStreamOrdering value is published — declare "
                    "ScheduleWorldBucketerDispatches(frame) BEFORE the world pass",
                    viewId);

            if (shadowArr.IsValid())
                p.Read(shadowArr, RenderGraph::RGTextureRead::Sampled);
            // PCSS min/max pyramid: the same compute->fragment edge as the RT
            // mask below, and even more load-bearing — the PCSS early-out reads
            // the pyramid through a BINDLESS index, so there is no by-name
            // binding here at all and this read is the ONLY thing ordering the
            // reduction dispatches before the draw that samples them. Whole
            // resource (every mip, every cascade layer): the early-out picks its
            // level per pixel. The pyramid is GENERAL-resident, so the barrier
            // is a memory dependency, not a layout change.
            if (wantsShadows && vfr.ShadowPcssPyramid.IsValid())
                p.Read(vfr.ShadowPcssPyramid, RenderGraph::RGTextureRead::Sampled);
            // Glass transmittance cascade: declare the sampled read so the RG inserts
            // the RenderTarget->ShaderRead transition + orders the tint cascade writes
            // before this world sample (the binding alone forms no producer->consumer edge).
            if (vfr.TransmittanceShadowArray.IsValid())
                p.Read(vfr.TransmittanceShadowArray, RenderGraph::RGTextureRead::Sampled);
            if (vfr.MsmMoments.IsValid())
                p.Read(vfr.MsmMoments, RenderGraph::RGTextureRead::Sampled);
            // RT shadow-mask: the declared read forms the compute->fragment
            // (StorageWrite->Sampled) edge that orders the ray-query mask pass
            // before this draw — the by-name binding alone forms no edge.
            if (vfr.RTShadowMask.IsValid())
                p.Read(vfr.RTShadowMask, RenderGraph::RGTextureRead::Sampled);
            if (vfr.ScreenSpaceShadowMask.IsValid())
                p.Read(vfr.ScreenSpaceShadowMask, RenderGraph::RGTextureRead::Sampled);
            if (vfr.AreaShadowMap.IsValid())
                p.Read(vfr.AreaShadowMap, RenderGraph::RGTextureRead::Sampled);
            if (vfr.SpotShadowMap.IsValid())
                p.Read(vfr.SpotShadowMap, RenderGraph::RGTextureRead::Sampled);
            if (vfr.PointShadowMap.IsValid())
                p.Read(vfr.PointShadowMap, RenderGraph::RGTextureRead::Sampled);
            // IBL bake -> world edge: these Reads land on the same ids the IBLGen
            // bake's StorageWrites produced (dedup-by-handle), inserting the
            // StorageWrite->Sampled barrier and ordering the bake before this draw.
            if (vfr.IblIrradiance.IsValid())
                p.Read(vfr.IblIrradiance, RenderGraph::RGTextureRead::Sampled);
            if (vfr.IblPrefilter.IsValid())
                p.Read(vfr.IblPrefilter, RenderGraph::RGTextureRead::Sampled);
            if (vfr.IblBrdfLut.IsValid())
                p.Read(vfr.IblBrdfLut, RenderGraph::RGTextureRead::Sampled);
            // ge_gtao samples the AO node's storage-written output: this read is
            // the compute->fragment RAW edge AND the General->ShaderReadOnly
            // transition that matches the descriptor's SHADER_READ_ONLY claim
            // (VUID-vkCmdDraw-None-09600 fired every frame without it — the
            // by-name binding alone forms no edge). Gated exactly like the
            // ge_gtao binding in BuildPassResourcesRG.
            if (targets.GTAO.IsValid() && HasKeyword(passKeywords, MaterialKeyword::GTAO))
                p.Read(targets.GTAO, RenderGraph::RGTextureRead::Sampled);
            // ge_ddgiResolveRough/Glossy sample the DDGI glossy-resolve
            // compute outputs: same compute->fragment RAW edge + layout
            // transition the ge_gtao read above provides, gated exactly like
            // their bindings in BuildPassResourcesRG.
            if (targets.DDGIResolveRough.IsValid() && targets.DDGIResolveGlossy.IsValid() &&
                targets.DDGIResolveIrradiance.IsValid() &&
                HasKeyword(passKeywords, MaterialKeyword::DDGI))
            {
                p.Read(targets.DDGIResolveRough, RenderGraph::RGTextureRead::Sampled);
                p.Read(targets.DDGIResolveGlossy, RenderGraph::RGTextureRead::Sampled);
                p.Read(targets.DDGIResolveIrradiance, RenderGraph::RGTextureRead::Sampled);
            }
            DeclareDdgiGatherReads(p, ddgiReads);
            // ge_sceneColor samples the transmission grab: the read is the
            // copy->draw RAW edge the grab's contract requires (SceneColorGrab.h)
            // — without it ordering rides only the scheduler's declaration-order
            // tie-break. Gated exactly like the ge_sceneColor binding.
            if (targets.SceneGrab.IsValid() &&
                HasKeyword(passKeywords, MaterialKeyword::SceneColorGrab))
                p.Read(targets.SceneGrab, RenderGraph::RGTextureRead::Sampled);
            // Producer-emitted forward draws that sample render-graph textures
            // descriptor-direct registered their imports at emit time
            // (EmitForwardSampledRead — ocean cascades in probe-face captures).
            // Declaring them here orders the writing sim before this draw and
            // restores ShaderReadOnly; without it the draw samples the sim's
            // Storage-state output (the export batch only lands at end of
            // frame). SampledVertex: emitted draws can displace vertices. The
            // stamp check matches the emit's import frame (RGTexture ids are
            // frame-local; a foreign frame must not consume them).
            {
                const auto& pvRes = m_ViewRegistry.PerView(viewId);
                if (pvRes.ForwardSampledFor.IsFor(frame))
                {
                    for (const auto& texture : pvRes.ForwardSampledRG)
                    {
                        if (texture.IsValid())
                            p.Read(texture, RenderGraph::RGTextureRead::SampledVertex);
                    }
                }
                // Buffers a GPU-driven node reads as indirect args / index /
                // vertex-stage SSBOs (CBT terrain, terrain grass). Each carries
                // its access so the arg buffer gets an Indirect edge (not
                // Storage), ordering the producing compute pass before the draw,
                // and its own import frame, since nodes emit from the frame they
                // declare in.
                for (const auto& bufferRead : pvRes.ForwardSampledBufferRG)
                {
                    if (bufferRead.For.IsFor(frame) && bufferRead.Buffer.IsValid())
                        p.Read(bufferRead.Buffer, bufferRead.Access);
                }
            }
            // ge_sceneDepth samples the resolved depth when it is a DISTINCT
            // texture — that read needs its own edge (the AttachDepth below
            // covers only targets.Depth).
            if (targets.DepthResolved.IsValid() && targets.DepthResolved.Id != targets.Depth.Id)
                p.Read(targets.DepthResolved, RenderGraph::RGTextureRead::Sampled);
            // ge_prepassDepth samples the depth attachment itself, attached read-only: the sampled read
            // and the read-only attach meet in the DepthReadOnly layout, and the read orders the
            // fragment stage after the prepass's write.
            if (readsReliefDepth)
                p.Read(targets.Depth, RenderGraph::RGTextureRead::Sampled);
            // Precise pipeline-buffer reads (the old arm's blanket
            // ReadBuffer-over-everything): only Graph-valid entries — the
            // device-local pool imports (ClusterBuffer, LightIndexBuffer)
            // whose compute writers must order before this pass. Upload
            // allocs are host-coherent and get no edge.
            if (auto* instance = m_FrameOrchestrator.PipelineInstanceForFrame(frame))
            {
                if (const auto* fr = instance->FrameResourcesFor(&frame))
                {
                    fr->ForEachBufferBinding(
                        viewId,
                        [&](StringId, const Pipeline::PipelineBufferBindingRG& b)
                        {
                            if (b.Graph.IsValid())
                                p.Read(b.Graph, RenderGraph::RGBufferRead::Storage);
                        });
                }
            }
            // Local shadow data UBOs are upload-ring allocs (host-coherent): no
            // declaration needed.

            if (targets.Color.IsValid())
            {
                RenderGraph::RGAttachmentOps col{};
                if (view->targets.clearColor && !colorAlreadyInit &&
                    scope != WorldPassDrawScope::TransmissiveOnly)
                {
                    col.Load = RenderGraph::RGLoadOp::Clear;
                    col.Clear.Color[0] = view->targets.clearColorValue.r;
                    col.Clear.Color[1] = view->targets.clearColorValue.g;
                    col.Clear.Color[2] = view->targets.clearColorValue.b;
                    col.Clear.Color[3] = view->targets.clearColorValue.a;
                }
                else
                {
                    col.Load = RenderGraph::RGLoadOp::Load;
                }
                col.Store = RenderGraph::RGStoreOp::Store;

                if (collapseToResolve)
                    p.AttachColor(0, targets.Resolve, col, targets.ColorRange);
                else
                    p.AttachColor(0, targets.Color, col, targets.ColorRange);
            }
            // Feature-owned MRT slices, attached verbatim: formats, clears and
            // locations belong to the contributing provider.
            AttachExtraColors(p, targets);
            DeclareForwardFogReads(p, targets, passKeywords);
            if (targets.Depth.IsValid())
            {
                RenderGraph::RGAttachmentOps dops{};
                const bool clearDepthNow =
                    needsClear && view->targets.clearDepth && !suppressDepthClear;
                dops.Load = clearDepthNow ? RenderGraph::RGLoadOp::Clear : RenderGraph::RGLoadOp::Load;
                dops.Store = RenderGraph::RGStoreOp::Store;
                if (clearDepthNow)
                    dops.Clear.Depth = view->targets.clearDepthValue;
                // No stencil clear plumbing: D32 depth targets carry no stencil
                // aspect. The recorder derives the stencil ops from the depth
                // attach — load DontCare, store None when read-only else DontCare.
                // dops.Store is likewise advisory: a ReadOnly attach records None.
                p.AttachDepth(targets.Depth, dops,
                              depthReadOnly ? RenderGraph::RGDepthAccess::ReadOnly
                                            : RenderGraph::RGDepthAccess::ReadWrite,
                              targets.DepthRange);
            }
            p.Prepare([this, data, viewId, passKeywords, reliefDepthSource, scope, phase,
                       sliceCascadeIndex, prepassDepth, prepassDepthSampler](RenderGraph::RGPrepareContext& context)
            {
                return WorldPassPreparation(*this, *data, context, viewId, passKeywords, reliefDepthSource,
                    scope, phase, sliceCascadeIndex, prepassDepth, prepassDepthSampler).Prepare();
            });
        },
        [this, data, viewId, viewportW, viewportH, letterbox, flipViewportY](RenderGraph::RGContext& ctx)
        {
            auto* cl = ctx.Cmd;
            if (!cl)
                return;

            if (viewportW != 0 && viewportH != 0)
            {
                if (letterbox.active)
                {
                    cl->SetViewport(static_cast<float>(letterbox.x),
                                    static_cast<float>(letterbox.y),
                                    static_cast<float>(letterbox.width),
                                    static_cast<float>(letterbox.height));
                    cl->SetScissor(letterbox.x, letterbox.y, letterbox.width, letterbox.height);
                }
                else if (flipViewportY)
                {
                    // Flipped-height viewport (origin at the bottom): inverts the
                    // rasterized winding on both backends so the mirror view's
                    // geometry isn't back-face culled. Matches the shadow-debug
                    // overlay's y-flip form.
                    cl->SetViewport(0.0f, static_cast<float>(viewportH),
                                    static_cast<float>(viewportW), -static_cast<float>(viewportH));
                    cl->SetScissor(0, 0, viewportW, viewportH);
                }
                else
                {
                    cl->SetViewport(0.0f, 0.0f, static_cast<float>(viewportW),
                                    static_cast<float>(viewportH));
                    cl->SetScissor(0, 0, viewportW, viewportH);
                }
            }

            if (!data->HasAnyDraws)
                return;
            auto& binder = Materials().Binder();
            PassBindingContext pass{};
            pass.Cmd = cl;
            uint32_t attribDrawsIssued = 0;
            DrawGeometryBindings geometryBindings(*cl);

            const auto& producerNames = Rendering::DrawProducerRegistry::ScopeNames();
            for (const auto& draw : data->Draws)
            {
                const auto producer = draw.Entity ? Rendering::DrawProducerRegistry::kInstances : draw.Command.Producer;
                ctx.SetProfilerScope(producerNames[producer].data());
                RecordForwardDrawCommand(draw.Prepared, *this, pass, geometryBindings);
                if (draw.Entity)
                {
                    ++attribDrawsIssued;
                    PushMeshDrawIssuedDiagnostic("color", viewId, draw.MaterialIndex,
                                                 draw.MeshIndex, draw.MaxDrawCount, draw.RangeCount);
                }
            }
            ctx.SetProfilerScope(nullptr);
            if (DrawAttributionEnabled())
                LogDrawAttribution("color", viewId, data->EntityBatchCount, attribDrawsIssued,
                                   pass.PipelineBinds, pass.DescriptorBinds,
                                   data->Consolidated, data->LiveGroupCount);
            binder.EndPass(pass);
        });

    if (canResolveColor && result.Pass.IsValid())
    {
        const Rendering::Passes::MsaaResolveViewBuffers resolveView = PipelineMsaaResolveViewBuffers(
            m_FrameOrchestrator.PipelineInstanceForFrame(frame), frame, viewId, m_DefaultPlaceholderBuffer);
        Rendering::Passes::AddMsaaColorResolvePassRG(frame, targets.Color, targets.Resolve, targets.BloomSampleDelta,
                                                     resolveView, (passName + ".ColorResolve").c_str());
    }
    return result;
}

Rendering::RenderGraph::RGPass RenderServices::AddForwardCommandPassForView(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    const WorldPassTargetsRG& targets, MaterialKeyword passKeywords,
    std::span<const DrawCommand> commands,
    std::span<const Rendering::RenderGraph::RGTexture> sampledTextures,
    const char* passName,
    std::span<const ForwardBufferReadRG> sampledBuffers,
    Rendering::RenderGraph::RGDepthAccess depthAccess)
{
    namespace RenderGraph = Rendering::RenderGraph;

    if (commands.empty())
        return {};

    const ViewDesc* view = nullptr;
    for (const auto& v : m_ViewRegistry.GetViews())
    {
        if (v.id == viewId)
        {
            view = &v;
            break;
        }
    }
    if (!view || (!targets.Color.IsValid() && !targets.Depth.IsValid()))
        return {};

    auto& vfr = ViewFrameRGFor(frame, viewId);
    const bool wantsShadows =
        Rendering::HasKeyword(passKeywords, Rendering::MaterialKeyword::Shadows);
    const RenderGraph::RGTexture shadowArr =
        (wantsShadows && vfr.ShadowMapArray.IsValid()) ? vfr.ShadowMapArray : RenderGraph::RGTexture{};

    bool canResolveColor = false;
    bool collapseToResolve = false;
    if (targets.Color.IsValid() && targets.Resolve.IsValid() &&
        targets.Resolve.Id != targets.Color.Id)
    {
        const auto& colorDesc = frame.Graph().ResourceDesc(targets.Color.Id);
        const auto& resolveDesc = frame.Graph().ResourceDesc(targets.Resolve.Id);
        if (resolveDesc.SampleCount == 1u)
        {
            if (colorDesc.SampleCount > 1u)
                canResolveColor = colorDesc.Format == resolveDesc.Format;
            else if (targets.ResolveFromPipeline)
                collapseToResolve = true;
        }
    }

    uint32_t viewportW = 0;
    uint32_t viewportH = 0;
    {
        const RenderGraph::RGTexture vpTex =
            targets.Resolve.IsValid() ? targets.Resolve : targets.Color;
        if (vpTex.IsValid())
        {
            const auto& d = frame.Graph().ResourceDesc(vpTex.Id);
            viewportW = d.Width;
            viewportH = d.Height;
        }
    }
    const ViewLetterbox letterbox = m_ViewRegistry.GetViewLetterbox(viewId);
    const bool flipViewportY = m_ViewRegistry.GetViewWorldPassFlipY(viewId);

    // Raster domain: contributor draws land in the same jittered raster as the
    // world pass (no-op when TAA is off).
    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
        *cam.Ptr = m_ViewRegistry.ResolveJitteredCameraData(viewId, frame.FrameIndex(),
                                                            viewportW, viewportH);

    ResolvedPassResources resources =
        BuildPassResourcesRG(frame, viewId, passKeywords, &targets, cam.Buffer, cam.Offset);
    const DDGIProbeFeature::GatherReadsRG ddgiReads =
        ImportDdgiGatherReads(frame, GetFeature<DDGIProbeFeature>(), passKeywords);

    struct ForwardPassData
    {
        PassBindingContext Inputs;
        std::span<const DrawCommand> Commands;
        std::vector<PreparedForwardDraw> Prepared;
        size_t BaseTextureCount = 0;
        RenderGraph::RGTexture VolumetricFog{};
        SamplerHandle FogSampler{};
    };
    auto* data = frame.AllocatePassData<ForwardPassData>();
    data->Inputs.View = viewId;
    data->Inputs.PassKeywords = passKeywords;
    data->Inputs.PassResources = std::move(resources);
    data->BaseTextureCount = data->Inputs.PassResources.Textures.size();
    data->VolumetricFog = HasKeyword(passKeywords, MaterialKeyword::VolumetricFog)
                              ? targets.VolumetricFog : RenderGraph::RGTexture{};
    data->FogSampler = m_Textures->GetSampler(Rendering::SamplerPreset::LinearClamp);
    data->Commands = CaptureForwardCommands(frame, commands);
    std::string debugName = passName && passName[0] != '\0' ? passName : "ForwardCommands";
    debugName += "[";
    debugName += view->debugName && view->debugName[0] != '\0' ? view->debugName : "View";
    debugName += "#";
    debugName += std::to_string(static_cast<uint32_t>(viewId));
    debugName += "]";

    const RenderGraph::RGPass drawPass = frame.AddPass(
        debugName.c_str(), Rendering::PassPhase::kWorldRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            if (shadowArr.IsValid())
                p.Read(shadowArr, RenderGraph::RGTextureRead::Sampled);
            // PCSS min/max pyramid: same bindless-consumer edge as the entity
            // world pass — forward contributors run the same PCSS early-out, and
            // the bindless fetch forms no edge of its own.
            if (wantsShadows && vfr.ShadowPcssPyramid.IsValid())
                p.Read(vfr.ShadowPcssPyramid, RenderGraph::RGTextureRead::Sampled);
            // Glass transmittance cascade: declare the sampled read so the RG inserts
            // the RenderTarget->ShaderRead transition + orders the tint cascade writes
            // before this world sample (the binding alone forms no producer->consumer edge).
            if (vfr.TransmittanceShadowArray.IsValid())
                p.Read(vfr.TransmittanceShadowArray, RenderGraph::RGTextureRead::Sampled);
            if (vfr.MsmMoments.IsValid())
                p.Read(vfr.MsmMoments, RenderGraph::RGTextureRead::Sampled);
            // RT shadow-mask: same compute->fragment ordering edge as the
            // entity world pass — forward contributors sample the same mask.
            if (vfr.RTShadowMask.IsValid())
                p.Read(vfr.RTShadowMask, RenderGraph::RGTextureRead::Sampled);
            if (vfr.ScreenSpaceShadowMask.IsValid())
                p.Read(vfr.ScreenSpaceShadowMask, RenderGraph::RGTextureRead::Sampled);
            if (vfr.AreaShadowMap.IsValid())
                p.Read(vfr.AreaShadowMap, RenderGraph::RGTextureRead::Sampled);
            if (vfr.SpotShadowMap.IsValid())
                p.Read(vfr.SpotShadowMap, RenderGraph::RGTextureRead::Sampled);
            if (vfr.PointShadowMap.IsValid())
                p.Read(vfr.PointShadowMap, RenderGraph::RGTextureRead::Sampled);
            if (vfr.IblIrradiance.IsValid())
                p.Read(vfr.IblIrradiance, RenderGraph::RGTextureRead::Sampled);
            if (vfr.IblPrefilter.IsValid())
                p.Read(vfr.IblPrefilter, RenderGraph::RGTextureRead::Sampled);
            if (vfr.IblBrdfLut.IsValid())
                p.Read(vfr.IblBrdfLut, RenderGraph::RGTextureRead::Sampled);
            // ge_gtao: same compute->fragment edge + General->ShaderReadOnly
            // transition as the entity world pass (the by-name binding alone
            // forms no edge; VUID-vkCmdDraw-None-09600 otherwise). Gated
            // exactly like the ge_gtao binding in BuildPassResourcesRG.
            if (targets.GTAO.IsValid() && HasKeyword(passKeywords, MaterialKeyword::GTAO))
                p.Read(targets.GTAO, RenderGraph::RGTextureRead::Sampled);
            // ge_ddgiResolveRough/Glossy sample the DDGI glossy-resolve
            // compute outputs: same compute->fragment RAW edge + layout
            // transition the ge_gtao read above provides, gated exactly like
            // their bindings in BuildPassResourcesRG.
            if (targets.DDGIResolveRough.IsValid() && targets.DDGIResolveGlossy.IsValid() &&
                targets.DDGIResolveIrradiance.IsValid() &&
                HasKeyword(passKeywords, MaterialKeyword::DDGI))
            {
                p.Read(targets.DDGIResolveRough, RenderGraph::RGTextureRead::Sampled);
                p.Read(targets.DDGIResolveGlossy, RenderGraph::RGTextureRead::Sampled);
                p.Read(targets.DDGIResolveIrradiance, RenderGraph::RGTextureRead::Sampled);
            }
            DeclareDdgiGatherReads(p, ddgiReads);
            // ge_sceneColor: same copy->draw RAW edge as the entity world pass
            // (see the SceneColorGrab.h contract). Gated like the binding.
            if (targets.SceneGrab.IsValid() &&
                HasKeyword(passKeywords, MaterialKeyword::SceneColorGrab))
                p.Read(targets.SceneGrab, RenderGraph::RGTextureRead::Sampled);
            // ge_sceneDepth samples the resolved depth. The id-match arm skips the declaration
            // when it aliases the depth ATTACHMENT — but the attach contributes only its own
            // scope, and a read-only one is DepthRead alone (EARLY/LATE_FRAGMENT_TESTS +
            // DEPTH_STENCIL_ATTACHMENT_READ). A contributor that texelFetches ge_sceneDepth
            // (gpu_fog_particles.glsl) needs FRAGMENT_SHADER + SHADER_READ in the same consumer
            // scope, or the prior depth write is available to it but not VISIBLE. Declaring both
            // unions the stages and access masks; the layout meet resolves to DepthReadOnly,
            // legal for sampling and for the read-only attach alike. Gated on ReadOnly on
            // purpose: a sampled read beside a ReadWrite attach is the same-pass
            // sample-what-you-write feedback loop the barrier builder asserts on.
            if (targets.DepthResolved.IsValid() &&
                (targets.DepthResolved.Id != targets.Depth.Id ||
                 depthAccess == RenderGraph::RGDepthAccess::ReadOnly))
                p.Read(targets.DepthResolved, RenderGraph::RGTextureRead::Sampled);
            // SampledVertex: caller-supplied textures can feed vertex modifiers
            // (the ocean cascades displace vertices), so the consumer scope must
            // cover the vertex stage as well as fragment. Fragment-only entries
            // just get a slightly wider dst scope.
            for (const auto& texture : sampledTextures)
            {
                if (texture.IsValid())
                    p.Read(texture, RenderGraph::RGTextureRead::SampledVertex);
            }
            // Compute-written buffers the draw consumes (GPU-driven producers like
            // CBT: indirect args + index + vertex-stage SSBOs). The declared read is
            // what orders this pass after the producing compute pass and lands the
            // buffer barrier — the bind alone forms no edge.
            for (const auto& bufferRead : sampledBuffers)
            {
                if (bufferRead.Buffer.IsValid())
                    p.Read(bufferRead.Buffer, bufferRead.Access);
            }
            if (auto* instance = m_FrameOrchestrator.PipelineInstanceForFrame(frame))
            {
                if (const auto* fr = instance->FrameResourcesFor(&frame))
                {
                    fr->ForEachBufferBinding(
                        viewId,
                        [&](StringId, const Pipeline::PipelineBufferBindingRG& b)
                        {
                            if (b.Graph.IsValid())
                                p.Read(b.Graph, RenderGraph::RGBufferRead::Storage);
                        });
                }
            }

            if (targets.Color.IsValid())
            {
                RenderGraph::RGAttachmentOps col{};
                col.Load = RenderGraph::RGLoadOp::Load;
                col.Store = RenderGraph::RGStoreOp::Store;
                if (collapseToResolve)
                    p.AttachColor(0, targets.Resolve, col, targets.ColorRange);
                else
                    p.AttachColor(0, targets.Color, col, targets.ColorRange);
            }
            AttachExtraColors(p, targets);
            DeclareForwardFogReads(p, targets, passKeywords);
            if (targets.Depth.IsValid())
            {
                RenderGraph::RGAttachmentOps dops{};
                // Contributors always composite over an already-rasterized scene: never a
                // clear, so no view-level clear config reaches this pass. `dops.Store` is
                // advisory — a ReadOnly attach records RGStoreOp::None (preserve, write
                // nothing) whatever it says.
                dops.Load = RenderGraph::RGLoadOp::Load;
                dops.Store = RenderGraph::RGStoreOp::Store;
                p.AttachDepth(targets.Depth, dops, depthAccess, targets.DepthRange);
            }
            p.Prepare([this, data](RenderGraph::RGPrepareContext& ctx)
            {
                data->Prepared.clear();
                data->Inputs.PassResources.Textures.resize(data->BaseTextureCount);
                BindVolumetricFogVolume(ctx, data->VolumetricFog, data->FogSampler, data->Inputs.PassResources);
                data->Prepared.reserve(data->Commands.size());
                for (const auto& command : data->Commands)
                {
                    if (auto prepared = PrepareForwardDrawCommand(
                            command, command.PipelineSetCount, *this, data->Inputs, m_FrontFace, ctx))
                        data->Prepared.push_back(*prepared);
                }
                return true;
            });
        },
        [this, data, viewportW, viewportH, letterbox, flipViewportY](RenderGraph::RGContext& ctx)
        {
            auto* cl = ctx.Cmd;
            if (!cl)
                return;

            if (viewportW != 0 && viewportH != 0)
            {
                if (letterbox.active)
                {
                    cl->SetViewport(static_cast<float>(letterbox.x),
                                    static_cast<float>(letterbox.y),
                                    static_cast<float>(letterbox.width),
                                    static_cast<float>(letterbox.height));
                    cl->SetScissor(letterbox.x, letterbox.y, letterbox.width, letterbox.height);
                }
                else if (flipViewportY)
                {
                    cl->SetViewport(0.0f, static_cast<float>(viewportH),
                                    static_cast<float>(viewportW), -static_cast<float>(viewportH));
                    cl->SetScissor(0, 0, viewportW, viewportH);
                }
                else
                {
                    cl->SetViewport(0.0f, 0.0f, static_cast<float>(viewportW),
                                    static_cast<float>(viewportH));
                    cl->SetScissor(0, 0, viewportW, viewportH);
                }
            }

            auto& binder = Materials().Binder();
            PassBindingContext pass{};
            pass.Cmd = cl;

            constexpr uint32_t kForwardTrackedVertexSlots = 6;
            DrawGeometryBindings geometryBindings(*cl, kForwardTrackedVertexSlots);

            for (const auto& prepared : data->Prepared)
                RecordForwardDrawCommand(prepared, *this, pass, geometryBindings);

            binder.EndPass(pass);
        });
    if (canResolveColor && drawPass.IsValid())
    {
        const Rendering::Passes::MsaaResolveViewBuffers resolveView = PipelineMsaaResolveViewBuffers(
            m_FrameOrchestrator.PipelineInstanceForFrame(frame), frame, viewId, m_DefaultPlaceholderBuffer);
        Rendering::Passes::AddMsaaColorResolvePassRG(frame, targets.Color, targets.Resolve, targets.BloomSampleDelta,
                                                     resolveView, (debugName + ".ColorResolve").c_str());
    }
    return drawPass;
}

void RenderServices::RequestScreenSpaceShadowPipelines()
{
    if (!GetDevice())
        return;
    if (!m_ScreenSpaceShadows)
        m_ScreenSpaceShadows = std::make_unique<ScreenSpaceShadowPasses>(GetDevice());
    m_ScreenSpaceShadows->RequestPipelines();
}

} // namespace Engine::Renderer
} // namespace GameEngine
