#include "Engine/Rendering/2D/SpriteRenderFeature.h"
#include "Engine/Rendering/SpritePixelGrid.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/ViewExposureScale.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Assets/TextureCook.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/Pipeline/PipelineDescTranslator.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

namespace
{

constexpr uint32 kInstanceBinding = 0;
constexpr uint32 kAtlasBinding = 1;

/** Smallest ring-slot capacity worth allocating; keeps sparse scenes off the churn path. */
constexpr uint32 kMinInstanceCapacity = 64;

/** Six vertices (two triangles) per instanced sprite quad. */
constexpr uint32 kVerticesPerQuad = 6;

} // namespace

SpriteRenderFeature::SpriteRenderFeature(RenderServices& services)
    : m_ForwardSubscription(services.RegisterForwardEmit(
          [this](ForwardEmitContext& context) { EmitDraws(context); }, {.WritesDepth = false, .RequiresFrame = true}))
{
}

SpriteRenderFeature::~SpriteRenderFeature()
{
    ReleaseGpuState();
}

bool SpriteRenderFeature::Initialize(IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device)
        return false;
    m_Device = device;

    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg("Shaders/sprite2d.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("SpriteRenderFeature: failed to load sprite2d.shaderpkg: {}", loadErr);
        return false;
    }
    const auto vs = pkg.stageBytes.find("vs");
    const auto fs = pkg.stageBytes.find("fs");
    if (vs == pkg.stageBytes.end() || fs == pkg.stageBytes.end() || vs->second.empty() ||
        fs->second.empty())
    {
        Logger::Log::Warning("SpriteRenderFeature: sprite2d.shaderpkg missing vs/fs stage bytes.");
        return false;
    }

    m_Layout.debugName = "Sprite2D.Set0";
    m_Layout.bindings = {
        {kInstanceBinding, DescriptorType::StorageBuffer, 1, kShaderStageVertex},
        {kAtlasBinding, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
        {2, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
        {3, DescriptorType::StorageBuffer, 1, kShaderStageFragment},
        {5, DescriptorType::UniformBuffer, 1, kShaderStageVertex},
    };

    PipelineDesc pipeline{};
    pipeline.type = PipelineType::Graphics;
    pipeline.vertexShader = vs->second;
    pipeline.pixelShader = fs->second;
    pipeline.rasterizationSamples = 1;
    pipeline.SetCullingMode(CullModeFlagBits::None);
    pipeline.EnableDepthTest(true, CompareOp::GreaterOrEqual);
    pipeline.depthStencilState.depthWriteEnable = false;
    pipeline.EnableBlending(true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
    pipeline.pushConstantSize = sizeof(SpritePushConstants);
    pipeline.pushConstantStagesMask = kShaderStageVertex;
    pipeline.AddDynamicState(DynamicState::Viewport);
    pipeline.AddDynamicState(DynamicState::Scissor);
    pipeline.descriptorSetLayouts.push_back(m_Layout);
    pipeline.debugName = "Sprite2D";

    m_PipelineId = PipelineDescTranslator::InternGraphics(*device, pipeline);
    if (!m_PipelineId.IsValid())
    {
        Logger::Log::Warning("SpriteRenderFeature: failed to intern the sprite2d pipeline.");
        return false;
    }

    // Pixel art is the default authoring case; a linear filter bleeds
    // neighbouring frames across the sheet seam.
    m_Sampler = device->CreateSampler(SamplerDesc::PointClamp("Sprite2D_AtlasSampler"));
    m_LinearSampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Sprite2D_LinearSampler"));
    if (!m_Sampler.IsValid() || !m_LinearSampler.IsValid())
        return false;

    m_FramesInFlight = device->GetFramesInFlight();
    if (m_FramesInFlight == 0)
        m_FramesInFlight = 1;

    m_ShaderMeta = std::move(pkg.meta);
    if (!EnsureQuadIndices())
        return false;
    m_Initialized = true;
    return true;
}

bool SpriteRenderFeature::EnsureQuadIndices()
{
    if (m_QuadIndices.IsValid())
        return true;
    const uint16 indices[] = {0, 1, 2, 3, 4, 5};
    BufferDesc desc{};
    desc.size = sizeof(indices);
    desc.usage = static_cast<uint32>(BufferUsage::Index);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = "Sprite2D.QuadIndices";
    m_QuadIndices = m_Device->CreateBuffer(desc);
    if (!m_QuadIndices.IsValid())
        return false;
    m_Device->UpdateBuffer(m_QuadIndices, 0, sizeof(indices), indices);
    return true;
}

void SpriteRenderFeature::ReleaseGpuState()
{
    if (!m_Device)
        return;
    if (m_QuadIndices.IsValid())
        m_Device->DestroyBuffer(m_QuadIndices);
    m_QuadIndices = {};
    for (auto& [viewId, ring] : m_ViewRings)
        for (BufferHandle buffer : ring.Buffers)
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
    for (auto& [viewId, ring] : m_ViewRings)
        for (auto buffer : ring.Lights) if(buffer.IsValid()) m_Device->DestroyBuffer(buffer);
    m_ViewRings.clear();
}

void SpriteRenderFeature::OnDeviceRebuilt(IDevice* device)
{
    if (!device || !m_Initialized)
        return;
    m_Device = device;
    m_QuadIndices = {};
    EnsureQuadIndices();
    // The rebuild already freed every buffer these handles name, and their
    // persistent mappings dangle — forget them WITHOUT destroying, so the next
    // acquire re-creates and re-maps rather than writing through freed memory.
    m_ViewRings.clear();
    m_Sampler = device->CreateSampler(SamplerDesc::PointClamp("Sprite2D_AtlasSampler"));
    m_LinearSampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Sprite2D_LinearSampler"));
}

void SpriteRenderFeature::SetSpriteDraws(uint64 worldId, std::vector<SpriteDraw>& draws)
{
    m_WorldId = worldId;
    m_SpriteDraws.swap(draws);
}

BufferHandle SpriteRenderFeature::AcquireInstanceBuffer(uint32 viewId, uint32 instanceCount)
{
    ViewRing& ring = m_ViewRings[viewId];
    if (ring.Buffers.size() != m_FramesInFlight)
    {
        ring.Buffers.assign(m_FramesInFlight, {});
        ring.Capacity.assign(m_FramesInFlight, 0u);
        ring.Lights.assign(m_FramesInFlight, {});
        ring.LightCapacity.assign(m_FramesInFlight, 0);
    }

    const uint32 slot = m_Device->GetFrameIndex() % m_FramesInFlight;
    if (!ring.Buffers[slot].IsValid() || ring.Capacity[slot] < instanceCount)
    {
        if (ring.Buffers[slot].IsValid())
            m_Device->DestroyBuffer(ring.Buffers[slot]);
        uint32 capacity = kMinInstanceCapacity;
        while (capacity < instanceCount)
            capacity *= 2;
        BufferDesc bd{};
        bd.size = static_cast<uint64>(capacity) * sizeof(SpriteInstance);
        bd.usage = static_cast<uint32>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "Sprite2D_Instances";
        ring.Buffers[slot] = m_Device->CreateBuffer(bd);
        ring.Capacity[slot] = ring.Buffers[slot].IsValid() ? capacity : 0;
    }
    return ring.Buffers[slot];
}

void SpriteRenderFeature::BuildViewDraws(RenderServices& rs, uint32 viewId,
                                         std::vector<SpriteBatch>& outBatches)
{
    outBatches.clear();
    m_InstanceScratch.clear();
    if (!m_Initialized || m_SpriteDraws.empty())
        return;

    const PixelPerfectViewState pixelPerfect = rs.Views().GetViewPixelPerfect(viewId);
    const float32 worldUnitsPerTexel = pixelPerfect.Active ? pixelPerfect.WorldUnitsPerTexel : 0.0f;
    const auto* view = rs.Views().FindViewDesc(viewId);
    const auto* camera = view ? rs.Views().FindCameraData(view->cameraId) : nullptr;
    if (!camera || !HasSpriteDraws(view->worldId))
        return;
    const float32 forward[3] = {camera->view[0][2], camera->view[1][2], camera->view[2][2]};
    Vector4 frustumPlanes[6];
    ExtractFrustumPlanes(camera->viewProjRel, frustumPlanes);
    m_ViewDraws.clear();
    for (const auto& draw : m_SpriteDraws)
    {
        if ((draw.RenderLayers & view->ActiveRenderLayerMask().Bits) == 0u)
            continue;
        Vector3 center = ViewRelativePoint(*camera, draw.Center);
        if (worldUnitsPerTexel > 0.0f)
            SnapSpriteToPixelGrid(center.v, draw.AxisX, draw.AxisY, worldUnitsPerTexel, camera->viewRel.Data(),
                Mathematics::WorldPosition::FromSector(CameraRenderOriginSector(*camera), Mathematics::Vector3{}));
        const Vector3 halfExtent(std::abs(draw.AxisX[0]) + std::abs(draw.AxisY[0]),
                                 std::abs(draw.AxisX[1]) + std::abs(draw.AxisY[1]),
                                 std::abs(draw.AxisX[2]) + std::abs(draw.AxisY[2]));
        if (!TestAabbFrustum(center - halfExtent, center + halfExtent, frustumPlanes))
            continue;
        const Vector3 eyeOffset = draw.Center - CameraEye(*camera);
        const float32 depth = eyeOffset.x * forward[0] + eyeOffset.y * forward[1] + eyeOffset.z * forward[2];
        m_ViewDraws.push_back({&draw, MakeRenderSortKey(draw.Sorting, depth, CameraEye(*camera), forward), center});
    }
    std::sort(m_ViewDraws.begin(), m_ViewDraws.end(), [](const ViewDraw& a, const ViewDraw& b)
    {
        if (a.Sort != b.Sort) return RenderSortBefore(a.Sort, b.Sort);
        return a.Draw->Entity.id < b.Draw->Entity.id;
    });
    for (const ViewDraw& viewDraw : m_ViewDraws)
    {
        const SpriteDraw& draw = *viewDraw.Draw;
        // Re-resolve every frame: the texture service retires and re-mints the
        // GPU texture behind a GUID (cook adoption, hot-reload, eviction), so a
        // latched handle would sample whatever inherited the slot.
        const TextureHandle atlas = rs.Textures().GetOrUpload(draw.Texture);
        if (!atlas.IsValid())
            continue;

        if(!draw.NormalMap.IsNull())rs.Textures().DeclareTextureClassification(draw.NormalMap,TextureColorSpace::Linear,TextureCookUsage::Normal);
        const TextureHandle normal=draw.NormalMap.IsNull()?atlas:rs.Textures().GetOrUpload(draw.NormalMap);
        if(!normal.IsValid())continue;
        if (outBatches.empty() || !CanAppendSpriteBatch(outBatches.back(),atlas,normal,viewDraw.Sort,draw.LinearFiltering))
        {
            SpriteBatch batch{};
            batch.Entity = draw.Entity;
            batch.Atlas = atlas;
            batch.Normal = normal;
            batch.LinearFiltering = draw.LinearFiltering;
            batch.FirstInstance = static_cast<uint32>(m_InstanceScratch.size());
            // The draws arrive Z-sorted, so the run this batch opens is the
            // deepest it will contain — the key that orders it against tiles.
            batch.WorldZ = draw.Center.Z;
            batch.Sort = viewDraw.Sort;
            outBatches.push_back(batch);
        }
        ++outBatches.back().InstanceCount;

        SpriteInstance& inst = m_InstanceScratch.emplace_back();
        for (int32 i = 0; i < 3; ++i)
        {
            inst.Center[i] = viewDraw.Center[i];
            inst.AxisX[i] = draw.AxisX[i];
            inst.AxisY[i] = draw.AxisY[i];
        }
        inst.Surface[0]=draw.Lit?1.f:0.f;
        inst.Surface[1]=draw.NormalStrength;
        inst.Surface[2]=draw.NormalMap.IsNull()?0.f:1.f;
        inst.Surface[3]=float(m_Lights.size());
        inst.Center[3] = 0.0f;
        inst.AxisX[3] = 0.0f;
        inst.AxisY[3] = 0.0f;
        for (int32 i = 0; i < 4; ++i)
        {
            inst.UVRect[i] = draw.UVRect[i];
            inst.Tint[i] = draw.Tint[i];
        }
    }

    if (m_InstanceScratch.empty())
    {
        outBatches.clear();
        return;
    }

    const uint32 instanceCount = static_cast<uint32>(m_InstanceScratch.size());
    const BufferHandle buffer = AcquireInstanceBuffer(viewId, instanceCount);
    if (!buffer.IsValid())
    {
        outBatches.clear();
        return;
    }
    m_Device->UpdateBuffer(buffer, 0, instanceCount * sizeof(SpriteInstance),
                           m_InstanceScratch.data());

    auto& ring=m_ViewRings[viewId];
    const uint32 slot=m_Device->GetFrameIndex()%m_FramesInFlight;
    const size_t lightCount=std::max(size_t(1),m_Lights.size());
    if(!ring.Lights[slot].IsValid() || ring.LightCapacity[slot]<lightCount) {
        if(ring.Lights[slot].IsValid())m_Device->DestroyBuffer(ring.Lights[slot]);
        BufferDesc bd{};bd.size=lightCount*sizeof(SpriteLight);
        bd.usage=static_cast<uint32>(BufferUsage::Storage);bd.memoryUsage=BufferMemoryUsage::Upload;
        bd.debugName="Sprite2D_Lights";
        ring.Lights[slot]=m_Device->CreateBuffer(bd);
        ring.LightCapacity[slot]=lightCount;
    }
    if(!ring.Lights[slot].IsValid()){outBatches.clear();return;}
    m_LightScratch.clear();
    for (const auto& light : m_Lights)
    {
        SpriteLight packed = light.Gpu;
        const Vector3 relative = ViewRelativePoint(*camera, light.Position);
        for (int32 axis = 0; axis < 3; ++axis)
            packed.PositionType[axis] = relative[axis];
        m_LightScratch.push_back(packed);
    }
    if (!m_LightScratch.empty())
        m_Device->UpdateBuffer(ring.Lights[slot], 0, m_LightScratch.size() * sizeof(SpriteLight), m_LightScratch.data());
    for (SpriteBatch& batch : outBatches) {batch.Instances=buffer;batch.Lights=ring.Lights[slot];}
}

void SpriteRenderFeature::EmitDraws(ForwardEmitContext& context)
{
    if (!context.Services || !context.Frame || context.Purpose != ForwardEmitPurpose::World ||
        !HasSpriteDraws() || (!IsInitialized() && !Initialize(context.Device)) || !EnsureQuadIndices())
        return;
    auto& services = *context.Services;
    const auto* view = services.Views().FindViewDesc(context.ViewId);
    if (!view || (view->purpose != ViewPurpose::Game && view->purpose != ViewPurpose::EditorScene))
        return;
    const auto* camera = services.Views().FindCameraData(view->cameraId);
    if (!camera || !HasSpriteDraws(view->worldId))
        return;
    auto& commands = m_ViewCommands[static_cast<uint32>(context.ViewId)];
    BuildViewDraws(services, static_cast<uint32>(context.ViewId), commands.Batches);
    const size_t count = commands.Batches.size();
    commands.Pushes.resize(count);
    commands.Buffers.resize(count);
    commands.Textures.resize(count);
    const float32 inverseExposure = 1.0f / ResolveViewExposureScale(services, *view);
    for (size_t index = 0; index < count; ++index)
    {
        const auto& batch = commands.Batches[index];
        auto& push = commands.Pushes[index];
        push.InvExposure[0] = inverseExposure;
        push.Batch[0] = batch.FirstInstance;
        auto& buffers = commands.Buffers[index];
        buffers = {{{HashStringId("Instances"), batch.Instances},
                    {HashStringId("Lights"), batch.Lights}}};
        const auto sampler = batch.LinearFiltering ? m_LinearSampler : m_Sampler;
        auto& textures = commands.Textures[index];
        textures = {{{HashStringId("uAtlas"), batch.Atlas, sampler},
                     {HashStringId("uNormal"), batch.Normal, sampler}}};
        SortedTransparentCommand submission{};
        submission.Sorting = batch.Sort;
        submission.Family = HashStringId("Sprite2D");
        submission.Entity = batch.Entity;
        submission.SubOrder = static_cast<uint32>(index);
        auto& command = submission.Command;
        command.InternedPipeline = m_PipelineId;
        command.PipelineMeta = &m_ShaderMeta;
        command.Geometry.AltGeom.IB = m_QuadIndices;
        command.Geometry.AltGeom.IndexType = IndexType::Uint16;
        command.IndexCount = 6;
        command.InstanceCount = batch.InstanceCount;
        command.Bindings.Buffers = buffers;
        command.Bindings.Textures = textures;
        command.Bindings.PushConstants = std::as_bytes(std::span(&push, 1));
        services.EmitSortedTransparentCommand(context.ViewId, submission);
        for (const auto& texture : textures)
            services.EmitForwardSampledRead(*context.Frame, context.ViewId,
                context.Frame->ImportExternalTexture("Sprite2D.Texture", texture.Texture,
                                                     ResourceState::ShaderResource));
        for (const auto& buffer : buffers)
            services.EmitForwardSampledBufferRead(*context.Frame, context.ViewId,
                context.Frame->ImportExternalBuffer("Sprite2D.Storage", buffer.Buffer),
                RenderGraph::RGBufferRead::Storage, ForwardBufferReaders::WorldPass);
    }
}

} // namespace GameEngine::Engine::Renderer
