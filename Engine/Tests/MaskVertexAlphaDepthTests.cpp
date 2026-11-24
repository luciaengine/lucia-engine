// An alpha-masked surface whose opacity comes from vertex alpha must be cut the
// same way by the depth passes and by the colour pass.
//
// The colour pass discards a Mask fragment whose opacity (albedo alpha x base
// colour alpha x vertex alpha) is below the cutoff. When a depth pass decides
// the same coverage without the vertex alpha, it keeps those fragments: the
// prepass writes the surface's depth over the area the colour pass then
// discards, the surface behind it fails the depth test there, and the user sees
// a hole in the clear colour; the shadow passes cast the cut-out area's shadow.
//
// The rendered tests draw that case through the engine's own world path: a
// real Mask material and mesh, the GPU scatter, the depth prepass the depth
// recorder records, and a depth readback. One draws a material that reads the
// vertex alpha, the other one that ignores vertex colour, whose depth draw must
// leave the cut-out area covered as its colour pass does. The rule test pins
// which depth draws read the colour stream, so the cost stays on the draws whose
// coverage needs it.

#include <gtest/gtest.h>

#include "../Source/Engine/Rendering/DepthPrepassLineage.h"
#include "../Source/Engine/Rendering/IndexedDepthDraw.h"
#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/FeatureDeclareContext.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/MeshPoolGroupPlan.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/SkinPaletteAtlas.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Platform/Environment.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include "StagedTestPaths.h"
#include "TestCameraEye.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace GameEngine::Engine::Renderer
{
// Hold the existing pending-bind boundary, then publish a completed GPU upload.
struct TextureCoverageUploadTestAccess
{
    static void Hold(TextureService& service, const GUID& texture, const GUID& material)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        service.m_PendingTextureBinds.push_back({material, "albedoMap"_sid, texture});
    }

    static void Publish(TextureService& service, const GUID& texture, TextureHandle uploaded)
    {
        service.m_TextureGPUCache.PublishOrAdopt(texture, TextureGPUCache::Entry{uploaded, {}});
    }
};
}

namespace
{

constexpr uint32_t kTargetSize = 64u;
constexpr uint64_t kWorldId = 11u;
// Reverse-Z depth under the identity camera: NDC z is the world z, and the
// larger value is the nearer surface. The masked card sits in front of the floor.
constexpr float kCardDepth = 0.75f;
constexpr float kFloorDepth = 0.25f;
constexpr float kDepthTolerance = 1e-3f;
// Pixels well inside each region: the card's vertex-alpha-0 half, its
// vertex-alpha-1 half, and the floor outside the card.
constexpr uint32_t kCutOutX = kTargetSize / 4u;
constexpr uint32_t kKeptX = kTargetSize * 3u / 4u;
constexpr uint32_t kCentreY = kTargetSize / 2u;
constexpr uint32_t kFloorX = 1u;
// The prepass variant of a material compiles on the first frame that draws it
// and publishes at the next frame begin; a few frames cover both materials.
constexpr int kMaxFrames = 8;
constexpr float kRestoredRigidDepth = 0.6f;
constexpr float kStaleRigidDepth = 0.95f;

enum class RestoreMode
{
    None,
    Failed,
    Succeeded,
    RigidRefill,
    PendingPublisher,
    PendingTexture,
    MissingRigidGroup,
    MissingSkinnedGroup
};

struct DepthProbe
{
    uint32_t X;
    uint32_t Y;
    float Depth;
};

struct ShadowReadbackProbes
{
    DepthProbe Kept{kKeptX, kCentreY, kCardDepth};
    DepthProbe CutOut{kCutOutX, kCentreY, kFloorDepth};
    DepthProbe Floor{kFloorX, kCentreY, kFloorDepth};
};

DepthProbe ProjectShadowProbe(const Matrix4x4& matrix, const Vector3& worldPosition)
{
    const Vector3 clip = matrix.TransformPoint(worldPosition);
    EXPECT_GT(clip.z, 0.0f);
    EXPECT_LT(clip.z, 1.0f);
    const float column = (clip.x * 0.5f + 0.5f) * kTargetSize;
    const float row = (0.5f - clip.y * 0.5f) * kTargetSize;
    EXPECT_GE(column, 0.0f);
    EXPECT_LT(column, static_cast<float>(kTargetSize));
    EXPECT_GE(row, 0.0f);
    EXPECT_LT(row, static_cast<float>(kTargetSize));
    return {static_cast<uint32_t>(std::clamp(column, 0.0f, static_cast<float>(kTargetSize - 1u))),
            static_cast<uint32_t>(std::clamp(row, 0.0f, static_cast<float>(kTargetSize - 1u))), clip.z};
}

ShadowReadbackProbes MakeShadowProbes(const Matrix4x4& matrix)
{
    ShadowReadbackProbes probes;
    probes.Kept = ProjectShadowProbe(matrix, Vector3(0.4f, 0.0f, kCardDepth));
    probes.CutOut = ProjectShadowProbe(matrix, Vector3(-0.4f, 0.0f, kCardDepth));
    probes.Floor = ProjectShadowProbe(matrix, Vector3(-0.95f, 0.0f, kFloorDepth));
    probes.CutOut.Depth = probes.Floor.Depth;
    return probes;
}

CameraData MakeShadowTestCamera()
{
    CameraData camera{};
    const auto view = Mathematics::MakeLookAtLH(Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 0.0f, 0.0f),
                                               Vector3(0.0f, -1.0f, 0.0f));
    const auto projection = Mathematics::MakeOrthographicLH_ZO_ReverseZ(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);
    const auto viewProjection = projection * view;
    camera.view = view;
    camera.viewRel = view;
    camera.proj = projection;
    camera.viewProj = viewProjection;
    camera.viewProjRel = viewProjection;
    camera.cameraPos[2] = 1.0f;
    return camera;
}

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 65536) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc desc{};
    desc.width = kTargetSize;
    desc.height = kTargetSize;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = usage;
    return desc;
}

Vertex MakeVertex(float x, float y, float z)
{
    Vertex v{};
    v.Position[0] = x;
    v.Position[1] = y;
    v.Position[2] = z;
    v.Normal[2] = -1.0f; // faces the camera, which looks down +Z
    return v;
}

// One axis-aligned rectangle at depth z, appended as two triangles.
void AppendRectangle(Mesh& mesh, float x0, float y0, float x1, float y1, float z)
{
    const uint32_t base = static_cast<uint32_t>(mesh.Vertices.size());
    mesh.Vertices.push_back(MakeVertex(x0, y0, z));
    mesh.Vertices.push_back(MakeVertex(x1, y0, z));
    mesh.Vertices.push_back(MakeVertex(x1, y1, z));
    mesh.Vertices.push_back(MakeVertex(x0, y1, z));
    for (const uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
        mesh.Indices.push_back(base + index);
}

void SetBounds(Mesh& mesh)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        mesh.MinBounds[axis] = mesh.Vertices[0].Position[axis];
        mesh.MaxBounds[axis] = mesh.Vertices[0].Position[axis];
        for (const Vertex& v : mesh.Vertices)
        {
            mesh.MinBounds[axis] = std::min(mesh.MinBounds[axis], v.Position[axis]);
            mesh.MaxBounds[axis] = std::max(mesh.MaxBounds[axis], v.Position[axis]);
        }
    }
}

// The masked card: its left half carries vertex alpha 0 and its right half
// vertex alpha 1, each half its own rectangle so no interpolation crosses the
// cut. White RGB, so only the alpha differs.
Mesh MakeCardMesh()
{
    Mesh mesh{};
    mesh.Name = "VertexAlphaCard";
    AppendRectangle(mesh, -0.8f, -0.8f, 0.0f, 0.8f, kCardDepth);
    AppendRectangle(mesh, 0.0f, -0.8f, 0.8f, 0.8f, kCardDepth);
    for (size_t i = 0; i < mesh.Vertices.size(); ++i)
    {
        const float alpha = i < 4 ? 0.0f : 1.0f;
        mesh.Color0.insert(mesh.Color0.end(), {1.0f, 1.0f, 1.0f, alpha});
    }
    SetBounds(mesh);
    return mesh;
}

enum class MaskProjection
{
    VertexAlpha,
    NearTexture,
    MipTailTexture
};

Mesh MakeTexturedCards(MaskProjection projection)
{
    Mesh mesh{};
    mesh.Name = "MaskedTextureCards";
    const uint32 columns = projection == MaskProjection::NearTexture ? 1u : 8u;
    constexpr float extent = 0.9f;
    const float spacing = 2.0f * extent / static_cast<float>(columns);
    for (uint32 y = 0; y < columns; ++y)
        for (uint32 x = 0; x < columns; ++x)
        {
            const float left = -extent + static_cast<float>(x) * spacing;
            const float bottom = -extent + static_cast<float>(y) * spacing;
            const size_t first = mesh.Vertices.size();
            AppendRectangle(mesh, left, bottom, left + spacing, bottom + spacing, kCardDepth);
            for (uint32 corner = 0; corner < 4; ++corner)
            {
                Vertex& vertex = mesh.Vertices[first + corner];
                vertex.TexCoords[0] = corner == 1 || corner == 2 ? 1.0f : 0.0f;
                vertex.TexCoords[1] = corner >= 2 ? 1.0f : 0.0f;
                mesh.Color0.insert(mesh.Color0.end(), {1.0f, 1.0f, 1.0f, 1.0f});
            }
        }
    SetBounds(mesh);
    return mesh;
}

TextureHandle UploadMaskTexture(IDevice& device)
{
    // A quarter of the base is opaque. Its tail mips hold that average everywhere,
    // so a cutoff would reject the entire card once it samples those levels.
    constexpr uint32 levels = 6;
    constexpr uint32 size = 1u << (levels - 1u);
    TextureDesc desc = TargetDesc(TextureFormat::RGBA8_UNORM,
        static_cast<uint32>(TextureUsage::ShaderResource | TextureUsage::TransferDst));
    desc.width = size;
    desc.height = size;
    desc.mipLevels = levels;
    const auto texture = device.CreateTexture(desc);
    if (!texture.IsValid())
        return {};

    std::array<std::vector<uint8>, levels> pixels;
    std::array<TextureMipUploadEntry, levels> entries;
    for (uint32 level = 0; level < levels; ++level)
    {
        const uint32 side = size >> level;
        auto& bytes = pixels[level];
        bytes.resize(size_t{side} * side * 4u, 255u);
        for (uint32 y = 0; y < side; ++y)
            for (uint32 x = 0; x < side; ++x)
                bytes[(y * side + x) * 4u + 3u] = level == 0
                    ? ((x % 8 < 4 && y % 8 < 4) ? 255u : 0u) : 64u;
        entries[level] = {bytes.data(), bytes.size(), side, side};
    }
    UploadTextureMips(&device, texture, entries.data(), levels, TextureFormat::RGBA8_UNORM,
                      "MaskVertexAlpha.TextureCoverage");
    return texture;
}

// The opaque floor behind the card, covering the whole target.
Mesh MakeFloorMesh()
{
    Mesh mesh{};
    mesh.Name = "Floor";
    AppendRectangle(mesh, -1.0f, -1.0f, 1.0f, 1.0f, kFloorDepth);
    SetBounds(mesh);
    return mesh;
}

MaterialDocument StandardDocument(const char* name, MaterialAlphaMode alphaMode)
{
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = alphaMode;
    doc.doubleSided = true;
    if (alphaMode == MaterialAlphaMode::Mask)
        doc.properties["alphaCutoff"] = 0.5f;
    return doc;
}

uint32_t AddInstance(RenderServices& rs, const MeshGPUEntry& entry, const Material& material,
                     const Mesh& mesh, bool castsShadows = false)
{
    GPUInstance instance{};
    instance.transform = Matrix4x4::Identity();
    instance.prevTransform = Matrix4x4::Identity();
    instance.normalMatrixCol0 = Vector3(1.0f, 0.0f, 0.0f);
    instance.normalMatrixCol1 = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    instance.normalMatrixCol2 = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    instance.renderLayerMask = 1u;
    instance.meshIndex = entry.gpuMeshIndex;
    instance.materialIndex = material.GetGpuSceneMaterialIndex();
    instance.flags = DepthClassInstanceFlagBits(
                         rs.Materials().GetMaterialDepthClass(material.GetGpuSceneMaterialIndex())) |
                     (PackWorldKey16(kWorldId) << 16);
    if (castsShadows)
        instance.flags |= kSubmissionFlagCastShadows;
    instance.lodBias = 0.0f;
    instance.boundingCenter = Vector3((mesh.MinBounds[0] + mesh.MaxBounds[0]) * 0.5f,
                                      (mesh.MinBounds[1] + mesh.MaxBounds[1]) * 0.5f,
                                      (mesh.MinBounds[2] + mesh.MaxBounds[2]) * 0.5f);
    instance.boundingRadius = 1.5f;
    return rs.GetGPUScene()->AddInstance(instance);
}

float DepthAt(const ViewReadbackResult& result, uint32_t x, uint32_t y)
{
    float depth = -1.0f;
    const size_t offset = (static_cast<size_t>(y) * result.width + x) * sizeof(float);
    if (offset + sizeof(float) <= result.pixels.size())
        std::memcpy(&depth, result.pixels.data() + offset, sizeof(float));
    return depth;
}

void SkinWithIdentityPalette(Mesh& mesh)
{
    mesh.Skinned = true;
    mesh.Joints0.assign(mesh.Vertices.size() * 4u, 0u);
    mesh.Weights0.assign(mesh.Vertices.size() * 4u, 0.0f);
    for (size_t vertex = 0; vertex < mesh.Vertices.size(); ++vertex)
        mesh.Weights0[vertex * 4u] = 1.0f;
}

bool PrepareWithMissingMeshGroup(RenderGraph::RGPrepareContext& context, PreparedDepthOnlyPass& prepared,
                                 RenderServices& services, uint32_t meshIndex, bool missingRigid)
{
    auto inputs = services.MakeDepthDrawServices();
    if (meshIndex >= inputs.MeshPoolGroups.size())
    {
        ADD_FAILURE() << "the positive control must publish both mesh-pool groups";
        return false;
    }
    std::vector<uint32_t> groups(inputs.MeshPoolGroups.begin(), inputs.MeshPoolGroups.end());
    EXPECT_NE(groups[meshIndex], MeshPoolGroupPlan::kAbsentGroup);
    groups[meshIndex] = MeshPoolGroupPlan::kAbsentGroup;
    inputs.MeshPoolGroups = groups;
    const bool ready = PrepareDepthOnlyPass(context, prepared, inputs);
    EXPECT_FALSE(GetPreparedDepthProducerState(prepared, CascadeCasters::All).Complete);
    EXPECT_EQ(GetPreparedDepthProducerState(prepared, CascadeCasters::Rigid).Complete, !missingRigid);
    return ready;
}

RenderGraph::RGTexture DeclareDepthComposition(RenderGraph::RGFrame& frame, RenderServices& services,
                                               ViewId viewId, RestoreMode mode,
                                               std::optional<uint32_t> missingMesh = {},
                                               DepthProducerState* producer = nullptr)
{
    const auto target = frame.ImportPersistentTexture(
        "MaskVertexAlpha.Composed",
        TargetDesc(TextureFormat::D32_FLOAT,
                   static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource |
                                         TextureUsage::TransferSrc)));
    auto camera = frame.AllocUpload<CameraData>();
    if (!target.IsValid() || !camera.Valid())
        return {};
    *camera.Ptr = services.Views().ResolveJitteredCameraData(
        viewId, frame.FrameIndex(), kTargetSize, kTargetSize);
    const auto inputs = services.MakeFeatureDeclareContext(frame, viewId, 1u, kTargetSize, nullptr, {});
    DepthOnlyPassParamsRG params{};
    params.ViewId = viewId;
    params.PassType = DepthPassType::Prepass;
    if (mode == RestoreMode::PendingPublisher || mode == RestoreMode::PendingTexture ||
        mode == RestoreMode::MissingRigidGroup ||
        mode == RestoreMode::MissingSkinnedGroup)
    {
        params.PassType = DepthPassType::ShadowCascade;
        params.DepthBiasEnable = true;
        params.DepthClampEnable = true;
        const auto* feature = services.GetFeature<ShadowMapRenderFeature>();
        const auto* fit = feature ? feature->GetCachedFrameData(viewId) : nullptr;
        if (!fit)
        {
            ADD_FAILURE() << "the real shadow culling fit must exist before raster preparation";
            return {};
        }
        *camera.Ptr = CameraData{};
        const auto identity = Matrix4x4::Identity();
        camera.Ptr->view = identity;
        camera.Ptr->proj = identity;
        camera.Ptr->viewProj = fit->LightVP[0];
        camera.Ptr->viewProjRel = fit->LightVPRel[0];
    }
    params.Casters = CascadeCasters::All;
    params.ViewportWidth = kTargetSize;
    params.ViewportHeight = kTargetSize;
    params.RasterizationSamples = 1u;
    params.Resources = services.BuildShadowPassResources(
        inputs.Seam, frame, viewId, camera.Buffer, camera.Offset);
    auto* prepared = AllocatePreparedDepthOnlyPass(frame, std::move(params));
    frame.AddPass("MaskVertexAlpha.SeedComposition", PassPhase::kWorldRender,
        [target, mode](RenderGraph::RGPassBuilder& builder)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Clear;
            ops.Store = RenderGraph::RGStoreOp::Store;
            ops.Clear.Depth = mode == RestoreMode::PendingPublisher || mode == RestoreMode::PendingTexture ||
                              mode == RestoreMode::MissingRigidGroup ||
                              mode == RestoreMode::MissingSkinnedGroup ? 0.0f :
                              mode == RestoreMode::Succeeded ? kRestoredRigidDepth : kStaleRigidDepth;
            builder.AttachDepth(target, ops, RenderGraph::RGDepthAccess::ReadWrite);
            builder.PreventCulling();
        }, [](RenderGraph::RGContext&) {});
    frame.AddPass("MaskVertexAlpha.ComposeDepth", PassPhase::kWorldRender,
        [target, prepared, &services, &inputs, missingMesh, mode, producer](RenderGraph::RGPassBuilder& builder)
        {
            builder.Prepare([prepared, &services, missingMesh, mode, producer](RenderGraph::RGPrepareContext& context)
            {
                if (missingMesh)
                    return PrepareWithMissingMeshGroup(context, *prepared, services, *missingMesh,
                                                        mode == RestoreMode::MissingRigidGroup);
                const bool ready = PrepareDepthOnlyPass(context, *prepared, services.MakeDepthDrawServices());
                if (producer)
                    *producer = GetPreparedDepthProducerState(*prepared, CascadeCasters::Rigid);
                return ready;
            });
            if (inputs.DrawStreamOrdering.IsValid())
                builder.Read(inputs.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Load;
            ops.Store = RenderGraph::RGStoreOp::Store;
            builder.AttachDepth(target, ops, RenderGraph::RGDepthAccess::ReadWrite);
            builder.PreventCulling();
        },
        [target, prepared, mode](RenderGraph::RGContext& context)
        {
            if (mode == RestoreMode::RigidRefill)
            {
                context.Cmd->SetScissor(0u, 0u, 1u, 1u);
                RecordPreparedRigidCascadeDepth(context, *prepared, context.GetTexture(target), 0u);
            }
            else if (mode == RestoreMode::PendingPublisher || mode == RestoreMode::PendingTexture ||
                     mode == RestoreMode::MissingRigidGroup ||
                     mode == RestoreMode::MissingSkinnedGroup)
                RecordPreparedDepthOnlyPass(context, *prepared);
            else
                RecordPreparedCascadeDepth(context, *prepared, context.GetTexture(target), 0u,
                                            mode == RestoreMode::Succeeded);
        });
    return target;
}

RenderGraph::RGTexture DeclarePublisherCascade(RenderGraph::RGFrame& frame, RenderServices& services,
                                               ViewId viewId)
{
    auto& feature = services.EnsureFeature<ShadowMapRenderFeature>();
    auto inputs = services.MakeFeatureDeclareContext(frame, viewId, 1u, kTargetSize, nullptr, {});
    // This granular entry point selects the retained-rigid and live-overlay path.
    inputs.HasPendingSkinnedInstances = true;
    EXPECT_TRUE(inputs.ViewNeedsCascades);
    EXPECT_TRUE(inputs.SkinPaletteAtlas.IsValid());
    feature.DeclareCascadePass(frame, services, inputs, 0u, "MaskVertexAlpha.PublisherCascade");
    bool rigidDeclared = false;
    for (uint32_t pass = 0; pass < frame.Graph().PassCount(); ++pass)
    {
        const char* name = frame.Graph().PassName(pass);
        rigidDeclared = rigidDeclared ||
                        (name && std::string_view(name) == "MaskVertexAlpha.PublisherCascade.RigidDepth");
    }
    // TransferSrc readback usage can replace the first physical and retire its fit.
    constexpr uint64_t kReadbackAdoptionFrames = 2;
    if (frame.FrameIndex() >= kReadbackAdoptionFrames)
    {
        EXPECT_TRUE(rigidDeclared) << "at frame " << frame.FrameIndex();
    }
    return services.ImportShadowMapArrayRG(frame, viewId, inputs.Seam);
}

// Draws the card, with the material `cardDocument` describes, over the opaque
// floor through the world path's depth prepass, and reads back the depth where
// the card's vertex alpha is 0. Frames repeat until the floor and the card's
// vertex-alpha-1 half both reach the depth target, so a variant still compiling
// cannot pass for a cut-out.
void DrawTheCardOverTheFloor(IDevice* device, const MaterialDocument& cardDocument, float& cutOutDepth,
                             RestoreMode restore = RestoreMode::None,
                             MaskProjection projection = MaskProjection::VertexAlpha)
{
    const std::filesystem::path shaderDir = TestPaths::StagedRenderingShadersDir();
    ASSERT_TRUE(std::filesystem::exists(shaderDir)) << "staged shader tree not found: " << shaderDir.string();
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device));
    ASSERT_NE(rs.GetGPUScene(), nullptr);
    const bool pendingPublisher = restore == RestoreMode::PendingPublisher;
    const bool pendingTexture = restore == RestoreMode::PendingTexture;
    const bool pendingScenario = pendingPublisher || pendingTexture;
    const bool missingGroup = restore == RestoreMode::MissingRigidGroup || restore == RestoreMode::MissingSkinnedGroup;
    const bool shadowScenario = pendingScenario || missingGroup;
    constexpr int kHeldPublisherFrames = 32;
    constexpr int kAbortedPublisherFrames = 8;

    const std::filesystem::path cacheRoot =
        std::filesystem::temp_directory_path() / ("ge_mask_vertex_alpha_" + GUID::Generate().ToString());
    MaterialBuildContext context{};
    context.AdapterShaderDir = shaderDir;
    context.CacheRoot = cacheRoot / "Shaders";
    context.IncludeDirs = {shaderDir};
    rs.Materials().SetMaterialBuildContext(context);

    Material* card = rs.Materials().RegisterMaterialFromDocument(GUID::Generate(), cardDocument);
    Material* floor = rs.Materials().RegisterMaterialFromDocument(
        GUID::Generate(), StandardDocument("OpaqueFloor", MaterialAlphaMode::Opaque));
    ASSERT_NE(card, nullptr);
    ASSERT_NE(floor, nullptr);
    if (pendingTexture)
    {
        testing::Test::RecordProperty("BaseCompilesPendingBeforeAlphaHold",
            static_cast<int>(rs.Materials().IsBaseCompileInFlight(card->GetGuid())) +
            static_cast<int>(rs.Materials().IsBaseCompileInFlight(floor->GetGuid())));
        rs.Materials().FlushAsyncMaterialCompiles();
        ASSERT_FALSE(rs.Materials().IsBaseCompileInFlight(card->GetGuid()));
        ASSERT_FALSE(rs.Materials().IsBaseCompileInFlight(floor->GetGuid()));
    }
    ASSERT_EQ(card->GetAlphaMode(), MaterialAlphaMode::Mask) << "the card material must stay masked";
    ASSERT_EQ(card->IgnoresVertexColor(), cardDocument.ignoreVertexColor);

    const bool measuresTextureCoverage = projection != MaskProjection::VertexAlpha;
    Mesh cardMesh = measuresTextureCoverage ? MakeTexturedCards(projection) : MakeCardMesh();
    if (pendingTexture)
        for (Vertex& vertex : cardMesh.Vertices)
        {
            vertex.TexCoords[0] = (vertex.Position[0] + 0.8f) / 1.6f;
            vertex.TexCoords[1] = (vertex.Position[1] + 0.8f) / 1.6f;
        }
    Mesh floorMesh = MakeFloorMesh();
    if (restore == RestoreMode::MissingRigidGroup)
        SkinWithIdentityPalette(cardMesh);
    if (restore == RestoreMode::MissingSkinnedGroup)
        SkinWithIdentityPalette(floorMesh);
    const MeshGPUHandle cardHandle = rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, cardMesh);
    const MeshGPUHandle floorHandle = rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, floorMesh);
    const MeshGPUEntry* cardEntry = rs.GetMeshGPURegistry().Find(cardHandle);
    const MeshGPUEntry* floorEntry = rs.GetMeshGPURegistry().Find(floorHandle);
    ASSERT_NE(cardEntry, nullptr);
    ASSERT_NE(floorEntry, nullptr);
    ASSERT_TRUE(HasFlag(cardEntry->vertexFlags, VertexAttributeFlags::HasColor))
        << "the card must upload its vertex colour stream";
    if (missingGroup)
    {
        ASSERT_EQ(IsSkinned(cardEntry->vertexFlags), restore == RestoreMode::MissingRigidGroup);
        ASSERT_EQ(IsSkinned(floorEntry->vertexFlags), restore == RestoreMode::MissingSkinnedGroup);
    }

    const CameraId cameraId = rs.Views().AllocateCamera("MaskVertexAlpha.Camera");
    CameraData camera{};
    camera.view = Mathematics::Matrix4x4::Identity();
    camera.proj = Mathematics::Matrix4x4::Identity();
    camera.viewProj = Mathematics::Matrix4x4::Identity();
    if (shadowScenario)
        camera = MakeShadowTestCamera();
    rs.Views().SetCameraData(cameraId, ::GameEngine::Tests::TestCameraWithEye(camera), {0.1f, 1000.0f});
    const ViewId viewId = rs.Views().AllocateView("MaskVertexAlpha.View", cameraId);
    rs.Views().SetViewRenderLayerMask(viewId, RenderLayerMask::Default());
    rs.Views().SetViewWorldId(viewId, kWorldId);
    ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearColorValue.a = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    const uint32_t cardInstance = AddInstance(rs, *cardEntry, *card, cardMesh, shadowScenario);
    const uint32_t floorInstance = AddInstance(rs, *floorEntry, *floor, floorMesh, shadowScenario);
    std::array<WorldSubmissionRecord, 2> submissions{};
    submissions[0] = {viewId, cardHandle, card, cardInstance, RenderLayerMask::All(), {}};
    submissions[1] = {viewId, floorHandle, floor, floorInstance, RenderLayerMask::All(), {}};
    const auto lastGood = card->GetGraphicsPipelineId();
    const uint64_t rigidEpoch = rs.RigidShadowCasterContentVersion(kWorldId);
    if (shadowScenario)
    {
        ASSERT_TRUE(lastGood.IsValid());
        for (auto& submission : submissions)
            submission.flags = kSubmissionFlagCastShadows;
        auto& feature = rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig config{};
        config.NumCascades = 1;
        config.Resolution = kTargetSize;
        ASSERT_TRUE(feature.Initialize(device, config));
        feature.SetMotionCap(0);
        if (pendingPublisher)
            Material::TestFactory::SetGraphicsPipelineId(*card, {});
    }

    const GUID alphaTexture = GUID::Generate();
    TextureHandle completedAlphaUpload{};
    if (pendingTexture)
    {
        TextureDesc desc = TargetDesc(TextureFormat::RGBA8_UNORM,
            static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst));
        desc.width = 2;
        desc.height = 1;
        completedAlphaUpload = device->CreateTexture(desc);
        ASSERT_TRUE(completedAlphaUpload.IsValid());
        const std::array<uint8_t, 8> pixels{255, 255, 255, 0, 255, 255, 255, 255};
        const TextureMipUploadEntry mip{pixels.data(), pixels.size(), 2u, 1u};
        UploadTextureMips(device, completedAlphaUpload, &mip, 1u, TextureFormat::RGBA8_UNORM,
                          "MaskVertexAlpha.DelayedAlphaUpload");
        device->WaitForIdle();
        rs.Textures().BindAwaitedTexture(card, "albedoMap"_sid);
        TextureCoverageUploadTestAccess::Hold(rs.Textures(), alphaTexture, card->GetGuid());
        ASSERT_FALSE(rs.Textures().IsMaterialTextureBindingComplete(card->GetGuid()));
    }

    TextureHandle maskTexture;
    if (measuresTextureCoverage)
    {
        maskTexture = UploadMaskTexture(*device);
        ASSERT_TRUE(maskTexture.IsValid());
        device->WaitForIdle();
        card->SetTexture("albedoMap"_sid, maskTexture);
        card->SetBindlessTextureIndex("albedoMap"_sid, rs.Textures().GetBindlessIndex(maskTexture));
    }

    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    std::optional<ViewReadbackResult> settled;
    std::optional<ViewReadbackResult> composedResult;
    bool shadowWarm = false;
    ShadowReadbackProbes probes;
    const int maxFrames = measuresTextureCoverage ? kMaxFrames * 2 : pendingScenario ? kHeldPublisherFrames + kAbortedPublisherFrames + kMaxFrames :
                          missingGroup ? kMaxFrames * 2 : kMaxFrames;
    for (int frameIndex = 0; frameIndex < maxFrames && !settled; ++frameIndex)
    {
        frame.BeginFrame(static_cast<uint64_t>(frameIndex));
        if (pendingTexture && frameIndex == kHeldPublisherFrames)
            TextureCoverageUploadTestAccess::Publish(rs.Textures(), alphaTexture, completedAlphaUpload);
        rs.BeginWorldDrawFrame();
        if (pendingTexture)
        {
            rs.Materials().FlushAsyncMaterialCompiles();
            ASSERT_EQ(rs.Textures().IsMaterialTextureBindingComplete(card->GetGuid()),
                      frameIndex >= kHeldPublisherFrames);
        }
        rs.GetPerFrameWritePool().BeginFrame(static_cast<uint32_t>(frameIndex));
        if (shadowScenario)
        {
            rs.GetSkinPaletteAtlas().BeginFrame(rs.GetPerFrameWritePool(),
                                                SkeletonStore::Instance().GetLivePaletteBoneCount());
            rs.NotifySkinPaletteContentChanged(kWorldId);
            ASSERT_EQ(rs.RigidShadowCasterContentVersion(kWorldId), rigidEpoch);
            ExtractedLight sun{};
            sun.castsShadows = 1u;
            sun.cascadeCount = 1u;
            sun.directionWS[1] = 0.0f;
            sun.directionWS[2] = -1.0f;
            rs.SubmitLight(kWorldId, sun);
            if (pendingPublisher && frameIndex == kHeldPublisherFrames)
            {
                const uint32_t version = card->GetVersion();
                const uint32_t revision = card->GetContentRevision();
                ShaderCacheKey key{};
                key.VariantKey = card->GetVariantKey();
                key.Source = MakeShaderSourceKey(card->GetCompileSpec().surfaceShaderPath,
                                                 card->GetCompileSpec().vertexModifierPath,
                                                 card->GetMaterialAssetPath());
                rs.Materials().EnqueueBasePipelinePublishForTesting(card->GetGuid(), {}, nullptr, key, lastGood);
                rs.Materials().FlushAsyncMaterialCompiles();
                ASSERT_EQ(card->GetGraphicsPipelineId(), lastGood);
                ASSERT_EQ(card->GetVersion(), version);
                ASSERT_EQ(card->GetContentRevision(), revision);
            }
        }
        rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(submissions));
        rs.BuildWorldBatchKeys();
        if (missingGroup)
        {
            const auto keys = rs.GetWorldDrawBuilder().GetBatchKeys(viewId);
            ASSERT_EQ(keys.size(), 2u);
            ASSERT_EQ(keys[0].mesh, cardHandle);
            ASSERT_EQ(keys[1].mesh, floorHandle);
        }
        rs.Materials().FinalizeFrameBuffers();
        if (rs.GetGPUScene()->IsDirty())
            rs.GetGPUScene()->FlushGPUBuffers();
        if (shadowScenario)
        {
            const auto* primary = SelectPrimaryDirectional(rs.GetWorldLights(kWorldId));
            ASSERT_NE(primary, nullptr);
            ShadowMapRenderFeature::DirectionalShadowSettings authored{};
            authored.MaxShadowDistance = 1.0f;
            authored.DepthBias = 0.0f;
            authored.NormalBias = 0.0f;
            rs.EnsureFeature<ShadowMapRenderFeature>().FitViewCascades(
                rs, viewId, kWorldId, rs.Views().ResolveCameraData(viewId), *primary, authored);
        }

        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
            "MaskVertexAlpha.Depth",
            TargetDesc(TextureFormat::D32_FLOAT,
                       static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));

        rs.ScheduleGpuSkinningAndRetarget(frame);
        rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);
        rs.ScheduleWorldBucketerDispatches(frame);
        if (shadowScenario)
        {
            const auto* fit = rs.EnsureFeature<ShadowMapRenderFeature>().GetCachedFrameData(viewId);
            ASSERT_NE(fit, nullptr);
            ASSERT_EQ(fit->NumCascades, 1u);
            probes = MakeShadowProbes(fit->LightVP[0]);
        }
        const RenderGraph::RGPass prepass = rs.AddWorldDepthPrepassForView(frame, viewId, depth, 0.0f);
        ASSERT_TRUE(prepass.IsValid());
        DepthProducerState observedProducer{};
        RenderGraph::RGTexture composed{};
        if (restore != RestoreMode::None)
        {
            std::optional<uint32_t> missingMesh{};
            if (missingGroup && frameIndex >= kMaxFrames)
            {
                ASSERT_TRUE(shadowWarm) << "both shadow casters must draw before a group is withheld";
                missingMesh = floorEntry->gpuMeshIndex;
            }
            composed = DeclareDepthComposition(frame, rs, viewId, restore, missingMesh,
                                               pendingTexture ? &observedProducer : nullptr);
            ASSERT_TRUE(composed.IsValid());
        }
        RenderGraph::RGTexture cached{};
        if (pendingScenario)
        {
            cached = DeclarePublisherCascade(frame, rs, viewId);
            ASSERT_TRUE(cached.IsValid());
        }
        // The prepass narrows the view's world-pass keywords to the instanced
        // fetch at execution. No world pass is declared, so the depth read
        // back is the prepass's alone and no other draw binds the colour
        // stream it must bind itself; the keywords a world pass records are
        // recorded here the same way.
        rs.Views().PerView(viewId).WorldPassKeywords = MaterialKeyword::Instanced;
        frame.MarkOutput(depth);

        const auto ticket = RequestTextureReadbackRG(device, frame, depth, "MaskVertexAlpha.DepthReadback");
        ASSERT_TRUE(ticket);
        const auto composedTicket = restore != RestoreMode::None
            ? RequestTextureReadbackRG(device, frame, composed, "MaskVertexAlpha.ComposedReadback")
            : nullptr;
        if (restore != RestoreMode::None)
        {
            ASSERT_TRUE(composedTicket);
        }
        const auto cachedTicket = pendingScenario
            ? RequestTextureReadbackRG(device, frame, cached, "MaskVertexAlpha.CachedPublisherReadback")
            : nullptr;
        const bool abortFrame = pendingScenario && frameIndex >= kHeldPublisherFrames &&
                                frameIndex < kHeldPublisherFrames + kAbortedPublisherFrames;
        if (abortFrame)
        {
            frame.AddPass("MaskVertexAlpha.AbortAfterPreparation", PassPhase::kFinalize,
                [cached](RenderGraph::RGPassBuilder& builder)
                {
                    builder.Read(cached);
                    builder.Prepare([](RenderGraph::RGPrepareContext&) { return false; });
                    builder.PreventCulling();
                }, [](RenderGraph::RGContext&) {});
        }
        frame.Execute();
        if (abortFrame)
        {
            EXPECT_EQ(frame.Stats().SubmissionsMade, 0u);
            EXPECT_EQ(frame.Stats().PreparationFailures, 1u);
            continue;
        }
        OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        device->WaitForIdle();

        ViewReadbackResult result{};
        ASSERT_TRUE(ticket->TryGet(result)) << "the depth readback did not resolve";
        ASSERT_EQ(result.width, kTargetSize);
        ASSERT_EQ(result.pixels.size(), static_cast<size_t>(kTargetSize) * kTargetSize * sizeof(float));
        const bool floorDrawn = std::abs(DepthAt(result, kFloorX, kCentreY) - kFloorDepth) < kDepthTolerance;
        if (measuresTextureCoverage)
        {
            if (frameIndex == maxFrames - 1)
            {
                ASSERT_TRUE(floorDrawn);
                settled = std::move(result);
            }
            continue;
        }
        const bool cardDrawn = std::abs(DepthAt(result, kKeptX, kCentreY) - kCardDepth) < kDepthTolerance;
        if (pendingPublisher && frameIndex == kHeldPublisherFrames - 1)
        {
            ASSERT_FALSE(card->GetGraphicsPipelineId().IsValid());
            ASSERT_TRUE(floorDrawn) << "the pending interval must include actual ready geometry";
            ASSERT_TRUE(cachedTicket);
            ViewReadbackResult retained{};
            ASSERT_TRUE(cachedTicket->TryGet(retained));
            EXPECT_NEAR(DepthAt(retained, probes.Floor.X, probes.Floor.Y), probes.Floor.Depth, kDepthTolerance);
            EXPECT_NEAR(DepthAt(retained, probes.Kept.X, probes.Kept.Y), probes.Floor.Depth, kDepthTolerance);
        }
        if (pendingTexture && frameIndex < kHeldPublisherFrames)
        {
            EXPECT_FALSE(observedProducer.Complete);
            if (frameIndex == kHeldPublisherFrames - 1)
            {
                ASSERT_TRUE(floorDrawn) << "floor depth=" << DepthAt(result, kFloorX, kCentreY);
                ASSERT_TRUE(cardDrawn) << "card depth=" << DepthAt(result, kKeptX, kCentreY);
                ASSERT_TRUE(composedTicket);
                ViewReadbackResult held{};
                ASSERT_TRUE(composedTicket->TryGet(held));
                EXPECT_NEAR(DepthAt(held, probes.Kept.X, probes.Kept.Y), probes.Kept.Depth, kDepthTolerance);
                EXPECT_NEAR(DepthAt(held, probes.CutOut.X, probes.CutOut.Y), probes.Kept.Depth, kDepthTolerance);
            }
            continue;
        }
        if (floorDrawn && cardDrawn)
        {
            settled = std::move(result);
            if (composedTicket)
            {
                ViewReadbackResult candidate{};
                ASSERT_TRUE(composedTicket->TryGet(candidate));
                ASSERT_EQ(candidate.width, kTargetSize);
                ASSERT_EQ(candidate.height, kTargetSize);
                ASSERT_EQ(candidate.pixels.size(), static_cast<size_t>(kTargetSize) * kTargetSize * sizeof(float));
                if (missingGroup)
                {
                    if (frameIndex < kMaxFrames)
                    {
                        if (frameIndex == kMaxFrames - 1)
                        {
                            ASSERT_NEAR(DepthAt(candidate, probes.Floor.X, probes.Floor.Y), probes.Floor.Depth, kDepthTolerance);
                            ASSERT_NEAR(DepthAt(candidate, probes.Kept.X, probes.Kept.Y), probes.Kept.Depth, kDepthTolerance);
                            shadowWarm = true;
                        }
                        settled.reset();
                        continue;
                    }
                    EXPECT_NEAR(DepthAt(candidate, probes.Kept.X, probes.Kept.Y), probes.Kept.Depth, kDepthTolerance);
                    EXPECT_NEAR(DepthAt(candidate, probes.Floor.X, probes.Floor.Y), 0.0f, kDepthTolerance);
                }
                if (pendingScenario)
                {
                    if (pendingTexture)
                    {
                        EXPECT_TRUE(observedProducer.Complete);
                    }
                    ASSERT_TRUE(cachedTicket);
                    ViewReadbackResult retained{};
                    ASSERT_TRUE(cachedTicket->TryGet(retained));
                    if (std::abs(DepthAt(candidate, probes.Kept.X, probes.Kept.Y) - probes.Kept.Depth) >= kDepthTolerance)
                    {
                        settled.reset();
                        continue;
                    }
                    EXPECT_NEAR(DepthAt(retained, probes.Kept.X, probes.Kept.Y), DepthAt(candidate, probes.Kept.X, probes.Kept.Y), kDepthTolerance);
                    EXPECT_NEAR(DepthAt(retained, probes.CutOut.X, probes.CutOut.Y), DepthAt(candidate, probes.CutOut.X, probes.CutOut.Y), kDepthTolerance);
                    EXPECT_NEAR(DepthAt(retained, probes.Floor.X, probes.Floor.Y), DepthAt(candidate, probes.Floor.X, probes.Floor.Y), kDepthTolerance);
                    candidate = std::move(retained);
                }
                composedResult = std::move(candidate);
            }
        }
    }

    ASSERT_TRUE(settled.has_value())
        << "the floor and the kept half of the card never both reached the depth target in " << maxFrames
        << " frames: the scene did not draw, so the cut-out cannot be judged";
    cutOutDepth = DepthAt(*settled, kCutOutX, kCentreY);
    if (measuresTextureCoverage)
    {
        constexpr uint32 border = 4u;
        constexpr uint32 span = kTargetSize - 2u * border;
        uint32 covered = 0;
        for (uint32 y = border; y < kTargetSize - border; ++y)
            for (uint32 x = border; x < kTargetSize - border; ++x)
                covered += std::abs(DepthAt(*settled, x, y) - kCardDepth) < kDepthTolerance;
        cutOutDepth = static_cast<float>(covered) / static_cast<float>(span * span);
    }
    if (restore != RestoreMode::None)
    {
        ASSERT_TRUE(composedResult.has_value());
        const bool restored = restore == RestoreMode::Succeeded;
        EXPECT_NEAR(DepthAt(*composedResult, probes.Kept.X, probes.Kept.Y),
                    restored ? kRestoredRigidDepth : probes.Kept.Depth, kDepthTolerance);
        EXPECT_NEAR(DepthAt(*composedResult, probes.Floor.X, probes.Floor.Y),
                    restored ? kRestoredRigidDepth : missingGroup ? 0.0f : probes.Floor.Depth, kDepthTolerance);
        cutOutDepth = DepthAt(*composedResult, probes.CutOut.X, probes.CutOut.Y);
        if (pendingTexture)
        {
            EXPECT_NEAR(cutOutDepth, probes.CutOut.Depth, kDepthTolerance);
        }
        if (restore == RestoreMode::RigidRefill)
        {
            for (uint32_t row = 0; row < kTargetSize; ++row)
                for (uint32_t column = 0; column < kTargetSize; ++column)
                    EXPECT_LT(DepthAt(*composedResult, column, row), kStaleRigidDepth - kDepthTolerance)
                        << "uncleared pixel " << column << "," << row;
        }
    }

    rs.Shutdown();
    if (maskTexture.IsValid())
        device->DestroyTexture(maskTexture);
    std::error_code ec;
    std::filesystem::remove_all(cacheRoot, ec);
}

} // namespace

TEST(IndexedDepthReuse, UsesTheFadeCoverageOfTheServedShader)
{
    using GameEngine::Engine::Renderer::StockMaskVertexAlpha;
    namespace R = GameEngine::Rendering;
    R::ShaderMeta head{};
    head.RequiredDefines = {R::kStockMaskCoverageDefine, "GE_DEPTH_ONLY_FRAGMENT"};
    R::ShaderMeta depthTail = head;
    depthTail.RequiredDefines.emplace_back("GE_LOD_CROSSFADE");
    R::ShaderMeta forwardTail{};
    forwardTail.RequiredDefines = {R::kStockMaskCoverageDefine, "GE_LOD_CROSSFADE"};
    EXPECT_EQ(StockMaskVertexAlpha(&head, true, false), false);
    EXPECT_FALSE(StockMaskVertexAlpha(&head, true, true));
    EXPECT_EQ(StockMaskVertexAlpha(&depthTail, true, true), false);
    EXPECT_FALSE(StockMaskVertexAlpha(&depthTail, true, false));
    EXPECT_FALSE(StockMaskVertexAlpha(&depthTail, false, true));
    EXPECT_EQ(StockMaskVertexAlpha(&forwardTail, false, true), false);
}

TEST(IndexedDepthReuse, MeshReplayRequiresTheStockVertexFetch)
{
    namespace R = GameEngine::Rendering;
    using GameEngine::Engine::Renderer::MatchesIndexedOwnerMeshVertexLayout;
    constexpr auto kFlags = R::VertexAttributeFlags::StandardMesh | R::VertexAttributeFlags::HasColor;
    R::GraphicsPipelineDesc pipeline;
    R::BuildVertexLayoutFromFlags(kFlags, pipeline);
    EXPECT_TRUE(MatchesIndexedOwnerMeshVertexLayout(pipeline, kFlags));

    const auto normal = std::find_if(pipeline.VertexAttributes.begin(), pipeline.VertexAttributes.end(),
        [](const auto& attribute) { return attribute.location == R::VertexLocation::Normal; });
    ASSERT_NE(normal, pipeline.VertexAttributes.end());
    normal->offset = 0u;
    EXPECT_FALSE(MatchesIndexedOwnerMeshVertexLayout(pipeline, kFlags));
}

TEST(IndexedDepthReuse, MeshReplayRejectsUnservedFragmentInputs)
{
    namespace R = GameEngine::Rendering;
    using GameEngine::Engine::Renderer::MatchesIndexedOwnerMeshFragmentInputs;
    R::StageIO normal{
        .Location = 1u, .Type = {.Kind = R::TypeKind::Vector, .Base = R::BaseType::Float, .VecSize = 3u}};
    R::StageMeta fragment;
    fragment.Inputs = {normal};
    R::StageMeta mesh;
    normal.Type.ArrayDims = {48u};
    mesh.Outputs = {normal};
    EXPECT_TRUE(MatchesIndexedOwnerMeshFragmentInputs(fragment, mesh));

    fragment.Inputs.front().Type.VecSize = 4u;
    EXPECT_FALSE(MatchesIndexedOwnerMeshFragmentInputs(fragment, mesh));
    fragment.Inputs.front().Type.VecSize = 3u;
    fragment.Inputs.front().Component = 1u;
    EXPECT_FALSE(MatchesIndexedOwnerMeshFragmentInputs(fragment, mesh));
    mesh.Outputs.front().Component = 1u;
    EXPECT_TRUE(MatchesIndexedOwnerMeshFragmentInputs(fragment, mesh));
    mesh.Outputs.front().Component = 0u;

    fragment.Inputs.front().Component = 0u;
    fragment.Inputs.push_back({.Location = 3u});
    EXPECT_FALSE(MatchesIndexedOwnerMeshFragmentInputs(fragment, mesh));
}

// A masked fragment reads the coverage seed at location 14, component 3 (mask_coverage.glsl). Both
// shipped replay mesh shaders write it there, so masked batches keep the owner replay.
TEST(IndexedDepthReuse, ShippedMeshReplayServesTheMaskCoverageSeed)
{
    namespace R = GameEngine::Rendering;
    using GameEngine::Engine::Renderer::MatchesIndexedOwnerMeshFragmentInputs;
    R::StageMeta fragment;
    fragment.Inputs = {{.Location = 14u, .Component = 3u, .Type = {.Kind = R::TypeKind::Scalar, .Base = R::BaseType::UInt}}};
    for (const char* name : {"indexed_depth_replay_mesh_forward.shaderpkg", "indexed_depth_replay_mesh_forward_color.shaderpkg"})
    {
        R::ShaderPackage package;
        ASSERT_TRUE(R::LoadShaderPkg((TestPaths::StagedRoot() / "Shaders" / name).string(), R::ShaderSourceKind::SpirV, package))
            << name;
        const auto mesh = package.meta.Stages.find("ms");
        ASSERT_NE(mesh, package.meta.Stages.end()) << name;
        EXPECT_TRUE(MatchesIndexedOwnerMeshFragmentInputs(fragment, mesh->second)) << name;
    }
}

TEST(IndexedDepthReuse, OwnerMaterialRowPreservesDonorConstantsAndRejectsInheritedRowOverrides)
{
    namespace R = GameEngine::Rendering;
    using GameEngine::Engine::Renderer::BindIndexedOwnerMaterialRow;
    using GameEngine::Engine::Renderer::kForwardHexTilingConstant;
    using GameEngine::Engine::Renderer::kForwardMaterialRowConstant;

    R::GraphicsPipelineDesc depth;
    depth.Specialization.emplace();
    depth.Specialization->AddConstant(kForwardHexTilingConstant, 0u);
    R::ShaderMeta shader;

    auto owner = depth;
    EXPECT_TRUE(BindIndexedOwnerMaterialRow(owner, shader, 18u));
    EXPECT_EQ(owner.Specialization->GetHash(), depth.Specialization->GetHash());

    shader.SpecConstants.push_back({.Id = kForwardMaterialRowConstant});
    EXPECT_TRUE(BindIndexedOwnerMaterialRow(owner, shader, std::nullopt));
    EXPECT_EQ(owner.Specialization->GetHash(), depth.Specialization->GetHash());

    auto expected = depth;
    expected.Specialization->AddConstant(kForwardMaterialRowConstant, 18u);
    EXPECT_TRUE(BindIndexedOwnerMaterialRow(owner, shader, 18u));
    EXPECT_EQ(owner.Specialization->GetHash(), expected.Specialization->GetHash());

    EXPECT_FALSE(BindIndexedOwnerMaterialRow(owner, shader, 41u));
    EXPECT_EQ(owner.Specialization->GetHash(), expected.Specialization->GetHash());
}

TEST(OrdinaryMaskedDraw, WhiteMapOverrideRequiresDeclaredConstantAndPreservesServedRow)
{
    ShaderMeta shader;
    GraphicsPipelineDesc pipeline;
    EXPECT_FALSE(AcceptsOrdinaryWhiteMapOverride(shader, pipeline, 18u));
    shader.SpecConstants.push_back({.Id = kStandardWhiteMapsConstant});
    EXPECT_TRUE(AcceptsOrdinaryWhiteMapOverride(shader, pipeline, 18u));

    pipeline.Specialization.emplace();
    pipeline.Specialization->AddConstant(kForwardHexTilingConstant, 0u);
    EXPECT_TRUE(AcceptsOrdinaryWhiteMapOverride(shader, pipeline, 18u));
    pipeline.Specialization->AddConstant(kForwardMaterialRowConstant, 18u);
    EXPECT_TRUE(AcceptsOrdinaryWhiteMapOverride(shader, pipeline, 18u));
    EXPECT_FALSE(AcceptsOrdinaryWhiteMapOverride(shader, pipeline, 41u));

    pipeline.Specialization->AddConstant(kStandardWhiteMapsConstant, 0u);
    EXPECT_FALSE(AcceptsOrdinaryWhiteMapOverride(shader, pipeline, 18u));
}

TEST(IndexedDepthReuse, WhiteSurfaceSlotsRejectTextureAndSamplerEdits)
{
    constexpr uint32_t kWhite = 17u;
    constexpr uint32_t kAuthoredTexture = 21u;
    auto material = Material::TestFactory::Create(GUID::Generate(), "Masked white slots", 16u);
    EXPECT_FALSE(HasDefaultWhiteSurfaceSlots(material, 0u));
    material.SetBindlessTextureIndex(TextureSlot::kAO, kWhite);
    material.SetBindlessTextureIndex(TextureSlot::kEmissive, kWhite);
    EXPECT_TRUE(HasDefaultWhiteSurfaceSlots(material, kWhite));

    for (const auto slot : {TextureSlot::kAO, TextureSlot::kEmissive})
    {
        material.SetBindlessTextureIndex(slot, kAuthoredTexture);
        EXPECT_FALSE(HasDefaultWhiteSurfaceSlots(material, kWhite));
        material.SetBindlessTextureIndex(slot, kWhite);
        EXPECT_TRUE(HasDefaultWhiteSurfaceSlots(material, kWhite));
    }

    material.SetSlotSamplerOverride("aoMap"_sid, SamplerPreset::LinearClamp);
    EXPECT_FALSE(HasDefaultWhiteSurfaceSlots(material, kWhite));
    material.SetSlotSamplerOverride("aoMap"_sid, std::nullopt);
    EXPECT_TRUE(HasDefaultWhiteSurfaceSlots(material, kWhite));
}

TEST(IndexedDepthReuse, DestinationColorLogicKeepsOriginalTriangleContributions)
{
    namespace R = GameEngine::Rendering;
    using GameEngine::Engine::Renderer::MatchesIndexedDepthRaster;
    R::GraphicsPipelineDesc depth;
    R::BuildVertexLayoutFromFlags(R::VertexAttributeFlags::StandardMesh, depth);
    depth.DepthStencil.depthTestEnable = true;
    depth.DepthStencil.depthWriteEnable = true;
    depth.DepthStencil.depthCompareOp = R::ReverseZ::kNearerOrEqual;
    auto forward = depth;
    forward.DepthStencil.depthWriteEnable = false;
    EXPECT_TRUE(MatchesIndexedDepthRaster(depth, forward, false));
    forward.ColorBlend.logicOpEnable = true;
    EXPECT_FALSE(MatchesIndexedDepthRaster(depth, forward, false));
}

namespace
{
void SetTestSwitch(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
} // namespace

TEST(IndexedDepthReuse, OwnerReplayRunsByDefaultOnVulkanAndStaysOptInOnMetal)
{
    using GameEngine::Rendering::GraphicsAPI;
    constexpr const char* kSwitch = "GE_TEST_INDEXED_DEPTH_OWNER_REPLAY";
    SetTestSwitch(kSwitch, nullptr);
    EXPECT_TRUE(IndexedDepthOwnerReplaySwitch(GraphicsAPI::Vulkan, true, kSwitch));
    EXPECT_FALSE(IndexedDepthOwnerReplaySwitch(GraphicsAPI::Metal, true, kSwitch));
    EXPECT_FALSE(IndexedDepthOwnerReplaySwitch(GraphicsAPI::Vulkan, false, kSwitch));

    SetTestSwitch(kSwitch, "0");
    EXPECT_FALSE(IndexedDepthOwnerReplaySwitch(GraphicsAPI::Vulkan, true, kSwitch));
    SetTestSwitch(kSwitch, "1");
    EXPECT_TRUE(IndexedDepthOwnerReplaySwitch(GraphicsAPI::Metal, true, kSwitch));
    EXPECT_FALSE(IndexedDepthOwnerReplaySwitch(GraphicsAPI::Metal, false, kSwitch));
    SetTestSwitch(kSwitch, nullptr);
}

TEST(IndexedDepthReuse, AnInterveningDepthWriterInvalidatesThePrepass)
{
    namespace RG = GameEngine::Rendering::RenderGraph;
    for (const bool rewritesDepth : {false, true})
    {
        RG::RGGraph graph;
        graph.BeginFrame();
        RG::RGResourceDesc desc;
        desc.Kind = RG::RGResourceKind::Texture;
        desc.Name = "CameraDepth";
        desc.Format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        const auto depth = graph.CreateResource(desc);
        const auto prepass = graph.AddPass({"CameraPrepass"});
        const auto inBetween = graph.AddPass({"InterveningPass"});
        const auto forward = graph.AddPass({"Forward"});
        graph.Write(prepass, depth, RG::RGAccess::DepthWrite);
        graph.Read(inBetween, depth, RG::RGAccess::DepthRead);
        if (rewritesDepth)
            graph.Write(inBetween, depth, RG::RGAccess::DepthWrite);
        graph.Read(forward, depth, RG::RGAccess::DepthRead);
        graph.PreventCulling(inBetween);
        graph.PreventCulling(forward);
        graph.AddOrderingEdge(inBetween, forward);
        graph.Compile();
        graph.Schedule();

        const RG::RGAttachmentRec producer{
            .Pass = prepass, .Tex = depth, .Ops = {.Load = RG::RGLoadOp::Clear}, .IsDepth = true};
        const RG::RGAttachmentRec consumer{
            .Pass = forward, .Tex = depth, .Ops = {.Store = RG::RGStoreOp::None},
            .IsDepth = true, .ReadOnly = true};
        const auto lineage = RG::CheckDepthPrepassLineage(graph, producer, consumer, false);
        EXPECT_EQ(static_cast<bool>(lineage), !rewritesDepth);
        EXPECT_EQ(lineage.BlockingPass, rewritesDepth ? inBetween : RG::kInvalidId);
        if (rewritesDepth)
        {
            auto continuation = producer;
            continuation.Pass = inBetween;
            continuation.Ops.Load = RG::RGLoadOp::Load;
            const std::array<const RG::RGAttachmentRec*, 1> continuations{&continuation};
            EXPECT_TRUE(RG::CheckDepthPrepassLineage(graph, producer, consumer, false, continuations));
            continuation.Ops.Load = RG::RGLoadOp::Clear;
            EXPECT_FALSE(RG::CheckDepthPrepassLineage(graph, producer, consumer, false, continuations));
        }
    }
}

TEST(IndexedDepthReuse, BothDepthContinuationsMustBeDeclaredInOrder)
{
    namespace RG = GameEngine::Rendering::RenderGraph;
    RG::RGGraph graph;
    graph.BeginFrame();
    RG::RGResourceDesc desc;
    desc.Kind = RG::RGResourceKind::Texture;
    desc.Name = "CameraDepth";
    desc.Format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    const auto depth = graph.CreateResource(desc);
    const auto prefix = graph.AddPass({"CameraPrepass"});
    const auto owner = graph.AddPass({"Owner"});
    const auto remainder = graph.AddPass({"Remainder"});
    const auto forward = graph.AddPass({"Forward"});
    graph.Write(prefix, depth, RG::RGAccess::DepthWrite);
    for (const auto pass : {owner, remainder})
    {
        graph.Read(pass, depth, RG::RGAccess::DepthRead);
        graph.Write(pass, depth, RG::RGAccess::DepthWrite);
    }
    graph.Read(forward, depth, RG::RGAccess::DepthRead);
    graph.PreventCulling(forward);
    graph.Compile();
    graph.Schedule();

    const RG::RGAttachmentRec producer{
        .Pass = prefix, .Tex = depth, .Ops = {.Load = RG::RGLoadOp::Clear}, .IsDepth = true};
    const RG::RGAttachmentRec first{.Pass = owner, .Tex = depth, .IsDepth = true};
    const RG::RGAttachmentRec second{.Pass = remainder, .Tex = depth, .IsDepth = true};
    const RG::RGAttachmentRec consumer{
        .Pass = forward, .Tex = depth, .Ops = {.Store = RG::RGStoreOp::None},
        .IsDepth = true, .ReadOnly = true};
    const std::array<const RG::RGAttachmentRec*, 2> ordered{&first, &second};
    EXPECT_TRUE(RG::CheckDepthPrepassLineage(graph, producer, consumer, false, ordered));
    const std::array<const RG::RGAttachmentRec*, 2> reversed{&second, &first};
    EXPECT_FALSE(RG::CheckDepthPrepassLineage(graph, producer, consumer, false, reversed));
    const std::array<const RG::RGAttachmentRec*, 1> missing{&first};
    const auto lineage = RG::CheckDepthPrepassLineage(graph, producer, consumer, false, missing);
    EXPECT_FALSE(lineage);
    EXPECT_EQ(lineage.BlockingPass, remainder);
}

// The card's left half is cut away by its vertex alpha, so the prepass depth
// there must be the floor's: the colour pass discards those fragments, and a
// prepass that kept them would hide the floor behind a surface that never draws.
TEST(MaskVertexAlphaDepth, ThePrepassLeavesTheVertexAlphaCutOutToTheSurfaceBehind)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    const MaterialDocument cardDocument = StandardDocument("VertexAlphaMask", MaterialAlphaMode::Mask);
    float cutOutDepth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), cardDocument, cutOutDepth));
    EXPECT_NEAR(cutOutDepth, kFloorDepth, kDepthTolerance)
        << "the depth where vertex alpha cuts the card away must be the floor's: the colour pass discards "
           "those fragments, so a prepass that writes the card's depth there leaves a hole";
    device->Shutdown();
}

// A Mask material that ignores vertex colour is not cut by the vertex alpha in
// its colour pass, so its prepass must keep the card's depth there too: a
// prepass that read the stream anyway would discard what the colour pass draws.
// Its base colour alpha of 0.9 passes the 0.5 cutoff everywhere and keeps it
// masked: DemoteMaskWithoutAlphaSource turns a Mask that ignores vertex colour
// into Opaque only when its base alpha is at least 0.999.
TEST(MaskVertexAlphaDepth, AMaskThatIgnoresVertexColourKeepsItsDepthWhereTheVertexAlphaIsZero)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    MaterialDocument cardDocument = StandardDocument("VertexColourIgnoringMask", MaterialAlphaMode::Mask);
    cardDocument.ignoreVertexColor = true;
    cardDocument.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.9f};
    float cutOutDepth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), cardDocument, cutOutDepth));
    EXPECT_NEAR(cutOutDepth, kCardDepth, kDepthTolerance)
        << "a material that ignores vertex colour draws its whole card in the colour pass, so the prepass must "
           "write the card's depth where the vertex alpha is 0";
    device->Shutdown();
}

TEST(MaskVertexAlphaDepth, ComposedMaskedCardsKeepNearCoverageInTheMipTail)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    auto document = StandardDocument("MipTailMask", MaterialAlphaMode::Mask);
    document.textures["albedoMap"] = {};
    float nearCoverage = 0.0f;
    float farCoverage = 0.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, nearCoverage, RestoreMode::None,
                                                   MaskProjection::NearTexture));
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, farCoverage, RestoreMode::None,
                                                   MaskProjection::MipTailTexture));
    RecordProperty("NearCoverage", std::to_string(nearCoverage));
    RecordProperty("MipTailCoverage", std::to_string(farCoverage));
    EXPECT_NEAR(nearCoverage, 0.25f, 0.07f);
    EXPECT_NEAR(farCoverage, nearCoverage, 0.09f);
    device->Shutdown();
}

TEST(MaskVertexAlphaDepth, FailedRigidRestoreReplaysPreparedRigidGeometry)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    const auto document = StandardDocument("RigidRestoreMask", MaterialAlphaMode::Mask);
    float depth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, depth, RestoreMode::Failed));
    EXPECT_NEAR(depth, kFloorDepth, kDepthTolerance);
    device->Shutdown();
}

TEST(MaskVertexAlphaDepth, RestoredRigidDepthIsNotOverwrittenByPreparedRigidGeometry)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    const auto document = StandardDocument("RigidRestoreMask", MaterialAlphaMode::Mask);
    float depth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, depth, RestoreMode::Succeeded));
    EXPECT_NEAR(depth, kRestoredRigidDepth, kDepthTolerance);
    device->Shutdown();
}

TEST(MaskVertexAlphaDepth, RigidRefillClearsTheWholePoisonedLayerAndRestoresTheViewport)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    const auto document = StandardDocument("RigidRefillMask", MaterialAlphaMode::Mask);
    float depth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, depth, RestoreMode::RigidRefill));
    EXPECT_NEAR(depth, kFloorDepth, kDepthTolerance);
    device->Shutdown();
}

TEST(MaskVertexAlphaDepth, PendingBasePublisherAndAbortedPreparationCannotSettleMissingRigidDepth)
{
    if (!Platform::EnvironmentSwitchEnabled("GE_SHADOW_RIGID_CASCADE_CACHE", false))
        GTEST_SKIP() << "The rigid cascade cache is opt-in; enable GE_SHADOW_RIGID_CASCADE_CACHE=1";
    auto device = CreateVulkanDeviceFast();
    if (!device || device->GetAPI() != GraphicsAPI::Vulkan)
        GTEST_SKIP() << "The rigid cascade cache requires Vulkan";
    const auto document = StandardDocument("PendingPublisherMask", MaterialAlphaMode::Mask);
    float depth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, depth, RestoreMode::PendingPublisher));
    EXPECT_GT(depth, 0.0f);
    device->Shutdown();
}

TEST(MaskVertexAlphaDepth, SkinnedBatchBeforeMissingRigidGroupCannotCertifyRigidDepth)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    const auto document = StandardDocument("MissingRigidGroupMask", MaterialAlphaMode::Mask);
    float depth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, depth, RestoreMode::MissingRigidGroup));
    EXPECT_NEAR(depth, 0.0f, kDepthTolerance);
    device->Shutdown();
}

TEST(MaskVertexAlphaDepth, RigidBatchBeforeMissingSkinnedGroupPreservesRigidCompleteness)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No graphics device available";
    const auto document = StandardDocument("MissingSkinnedGroupMask", MaterialAlphaMode::Mask);
    float depth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, depth, RestoreMode::MissingSkinnedGroup));
    EXPECT_NEAR(depth, 0.0f, kDepthTolerance);
    device->Shutdown();
}

// Which depth draws read the vertex colour: a Mask material's coverage fragment
// in every depth pass that evaluates its opacity, and nothing else, so an opaque
// material's depth and shadow draws keep the position-only layout. A material
// that ignores vertex colour opts out through its colour pass's layout, which
// has no Color stream (AMaskThatIgnoresVertexColourKeepsItsDepthWhereTheVertexAlphaIsZero).
TEST(MaskVertexAlphaDepth, OnlyAMaskedCoverageFragmentReadsTheVertexColour)
{
    constexpr DepthPassType kCoveragePasses[] = {
        DepthPassType::Prepass,     DepthPassType::ShadowCascade, DepthPassType::AreaShadow,
        DepthPassType::SpotShadow,  DepthPassType::PointShadow,   DepthPassType::DeformationMotion};
    for (const DepthPassType pass : kCoveragePasses)
    {
        EXPECT_TRUE(DepthPassReadsVertexColor(pass, MaterialAlphaMode::Mask))
            << "pass " << static_cast<int>(pass) << ": the coverage fragment must see the vertex alpha";
        EXPECT_FALSE(DepthPassReadsVertexColor(pass, MaterialAlphaMode::Opaque))
            << "pass " << static_cast<int>(pass) << ": an opaque draw keeps the position-only layout";
    }
    EXPECT_FALSE(DepthPassReadsVertexColor(DepthPassType::TransmittanceCascade, MaterialAlphaMode::Mask))
        << "the glass tint writes the transmission colour without evaluating opacity";
}

TEST(MaskVertexAlphaDepth, DelayedAlphaPublicationCannotCertifyFallbackCoverage)
{
    if (!Platform::EnvironmentSwitchEnabled("GE_SHADOW_RIGID_CASCADE_CACHE", false))
        GTEST_SKIP() << "The rigid cascade cache is opt-in; enable GE_SHADOW_RIGID_CASCADE_CACHE=1";
    const auto previousDirectory = std::filesystem::current_path();
    const auto workspace = std::filesystem::temp_directory_path() /
                           ("ge-shadow-alpha-" + GUID::Generate().ToString());
    std::filesystem::create_directories(workspace / "Assets");
    struct WorkspaceScope
    {
        std::filesystem::path Previous;
        std::filesystem::path Root;
        ~WorkspaceScope()
        {
            EngineCore::GetInstance().Shutdown();
            std::filesystem::current_path(Previous);
            std::error_code error;
            std::filesystem::remove_all(Root, error);
        }
    } scope{previousDirectory, workspace};
    auto& engine = EngineCore::GetInstance();
    ASSERT_FALSE(engine.IsInitialized());
    ScriptsConfig scripts;
    scripts.disableClr = true;
    scripts.enableHotReload = false;
    scripts.enableAsyncHotReload = false;
    scripts.enableAutoProjectGeneration = false;
    engine.SetScriptsConfig(scripts);
    ApplicationConfig config;
    config.WorkspaceDirectory = workspace.string();
    config.EnableEditor = false;
    ASSERT_TRUE(engine.Initialize(config));
    AssetSourceDesc shaderMount{};
    shaderMount.Alias = std::string(kAssetSourceAliasEditor);
    shaderMount.Root = TestPaths::StagedRoot();
    shaderMount.DerivedIdentity = true;
    shaderMount.RequiresScan = false;
    shaderMount.RegisterFileWatcher = false;
    shaderMount.IsImmutable = true;
    shaderMount.AcceptsMetadataWrites = false;
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderMount.Root / "Shaders/frustum_culling.shaderpkg"));
    ASSERT_TRUE(engine.GetAssetManager().RegisterSource(shaderMount));
    auto device = CreateVulkanDeviceFast();
    if (!device || device->GetAPI() != GraphicsAPI::Vulkan)
        GTEST_SKIP() << "The rigid cascade cache requires Vulkan";
    auto document = StandardDocument("DelayedAlphaMask", MaterialAlphaMode::Mask);
    document.ignoreVertexColor = true;
    document.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 0.9f};
    float depth = -1.0f;
    ASSERT_NO_FATAL_FAILURE(DrawTheCardOverTheFloor(device.get(), document, depth, RestoreMode::PendingTexture));
    EXPECT_GT(depth, 0.0f);
    EXPECT_LT(depth, 1.0f);
    device->Shutdown();
}
