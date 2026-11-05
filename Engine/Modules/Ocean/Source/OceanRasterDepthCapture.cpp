#include "Ocean/OceanRasterDepthCapture.h"
#include "Ocean/OceanShaderProgram.h"
#include "Ocean/OceanPreparedDispatch.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGRotatingImport.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;
namespace RG = ::GameEngine::Rendering::RenderGraph;

namespace
{
struct alignas(16) OceanRasterDepthParamsGPU
{
    float NearFarDeepResolution[4]{}; // x near/top padding, y far, z deep, w resolution
};
static_assert(sizeof(OceanRasterDepthParamsGPU) == 16);

bool NearlyEqual(float32 a, float32 b, float32 epsilon = 1e-4f)
{
    return std::fabs(a - b) <= epsilon;
}

float32 SnapToTexel(float32 value, float32 texelSize)
{
    if (texelSize <= 1e-6f)
        return value;
    return std::round(value / texelSize) * texelSize;
}

} // namespace

struct OceanRasterDepthCapture::OutputGeneration
{
    IDevice* Device = nullptr;
    RG::RGRotatingTexture Textures;
    uint32 Resolution = 0;
    ~OutputGeneration() { if (Device) Textures.Destroy(Device); }
};

struct OceanRasterDepthCapture::Snapshot
{
    std::shared_ptr<OutputGeneration> Generation;
    uint64 Serial = 0, AvailableEpoch = 0, World = 0;
    uint32 Resolution = 0, Mask = 0;
    float32 OriginX = 0, OriginZ = 0, SizeX = 1, SizeZ = 1, Deep = 60000, SeaLevel = 0, TopPadding = 0;

    TextureHandle Texture() const { return Generation ? Generation->Textures.Current(Serial) : TextureHandle{}; }
    bool Matches(const Snapshot& other) const
    {
        return Generation && Resolution == other.Resolution && Mask == other.Mask && World == other.World &&
               NearlyEqual(OriginX, other.OriginX) && NearlyEqual(OriginZ, other.OriginZ) &&
               NearlyEqual(SizeX, other.SizeX) && NearlyEqual(SizeZ, other.SizeZ) &&
               NearlyEqual(Deep, other.Deep) && NearlyEqual(SeaLevel, other.SeaLevel) &&
               NearlyEqual(TopPadding, other.TopPadding);
    }
};

struct OceanRasterDepthCapture::FramePlan
{
    Snapshot Proposed;
    // Do not destroy the previous generation while publishing the new snapshot in Record.
    std::shared_ptr<OutputGeneration> RetiredGeneration;
    RG::RGFrameStamp Owner;
    uint64 Epoch = 0;
    bool Convert = false, Recorded = false;
    RG::RGTexture Depth, Output;
    BufferHandle Params;
    uint64 ParamsOffset = 0;
    OceanPreparedDispatch Commands;
    BufferHandle ReadbackOutput;
    uint64 Reservation = 0;
};

OceanRasterDepthCapture::OceanRasterDepthCapture() : m_Committed(std::make_unique<Snapshot>()) {}

OceanRasterDepthCapture::~OceanRasterDepthCapture()
{
    if (m_Device)
    {
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Readback.Destroy(m_Device);
    }
    if (m_RenderServices && m_ViewAllocated)
    {
        if (m_ViewId != 0)
            m_RenderServices->Views().ReleaseView(m_ViewId);
        if (m_CameraId != 0)
            m_RenderServices->Views().ReleaseCamera(m_CameraId);
    }
}

bool OceanRasterDepthCapture::EnsurePipeline(IDevice& device)
{
    if (!m_PipelineLoadAttempted)
    {
        m_PipelineLoadAttempted = true;

        const fs::path shaderDir = OceanShaderDirectory("ocean_raster_depth_convert.comp");
        if (shaderDir.empty())
        {
            Logger::Log::Warning("OceanRasterDepthCapture: converter shader unavailable");
            return false;
        }

        ShaderProgramCompileRequest req{};
        req.debugName = "ocean_raster_depth_convert";
        req.baseDirectory = shaderDir;
        req.cacheRoot = fs::path(".Cache") / "Shaders";
        req.includeDirs = {shaderDir.parent_path()};
        req.stages = {{"cs", "ocean_raster_depth_convert.comp", "main", {}}};

        ShaderProgramCompileResult result{};
        std::string err;
        if (!LoadOceanShaderProgram(req, device.PreferredShaderSource(), result, &err))
        {
            if (!m_WarnedLoadFailed)
            {
                m_WarnedLoadFailed = true;
                Logger::Log::Warning("OceanRasterDepthCapture: converter compile failed: {}", err);
            }
            return false;
        }
        auto cs = result.stageBytes.find("cs");
        if (cs == result.stageBytes.end() || cs->second.empty())
            return false;

        m_Layout = {};
        m_Layout.debugName = "Ocean.RasterDepthConvert.Set0";
        auto addBinding = [&](uint32 binding, DescriptorType type, uint32 stages)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = type;
            b.count = 1;
            b.shaderStages = stages;
            m_Layout.bindings.push_back(b);
        };
        addBinding(0, DescriptorType::CombinedImageSampler, kShaderStageCompute);
        addBinding(1, DescriptorType::StorageImage, kShaderStageCompute);
        addBinding(2, DescriptorType::UniformBuffer, kShaderStageCompute);

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs->second));
        Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
        cd.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(m_Layout));
        cd.DebugName = "Ocean.RasterDepthConvert";
        m_PipelineId = device.InternComputePipeline(cd);
    }

    return m_PipelineId.IsValid();
}

bool OceanRasterDepthCapture::EnsureOutput(IDevice& device, uint32 resolution)
{
    if (!m_Sampler.IsValid())
        m_Sampler = device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_RasterWaterDepth_Sampler"));
    if (!m_Sampler.IsValid())
        return false;
    if (m_OutputGeneration && m_OutputGeneration->Resolution == resolution)
        return true;
    auto generation = std::make_shared<OutputGeneration>();
    generation->Device = &device;
    generation->Resolution = resolution;
    TextureDesc td{};
    td.width = td.height = resolution;
    td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
    td.format = static_cast<uint32>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::TransferSrc);
    td.initialState = ResourceState::ShaderResource;
    td.debugName = "Ocean_RasterWaterDepth";
    generation->Textures.Init(&device, td, 2u);
    if (!generation->Textures.Current(0).IsValid() || !generation->Textures.Current(1).IsValid())
        return false;
    m_OutputGeneration = std::move(generation);
    return true;
}

bool OceanRasterDepthCapture::EnsureReadbackRing(IDevice& device, uint32 resolution)
{
    resolution = std::clamp(resolution, 16u, 4096u);
    const uint32 ringCount = std::max(1u, device.GetFramesInFlight()) + 2u;
    if (m_Readback.IsInitialized() && m_Readback.Count() == ringCount &&
        m_ReadbackResolution == resolution)
    {
        return true;
    }

    BufferDesc bd{};
    bd.size = sizeof(float32) * size_t(resolution) * size_t(resolution);
    bd.usage = static_cast<uint32>(BufferUsage::TransferDst);
    bd.memoryUsage = BufferMemoryUsage::Readback;
    bd.debugName = "Ocean_RasterDepth_Readback";
    if (!m_Readback.Init(&device, bd, ringCount))
        return false;

    m_ReadbackResolution = resolution;
    return true;
}

void OceanRasterDepthCapture::InvalidateReadback()
{
    // Invalidation means DISCARD: in-flight readbacks must not resolve as
    // fresh after a settings toggle / capture retarget, or pre-invalidation
    // depth resurrects into SampleDepth on the re-enable frames.
    m_Readback.DropPendings();
    *m_Committed = {};
    m_Plan = nullptr;
    m_PlannedFrame = {};
    m_ClaimedEpoch = ~0ull;
    m_CaptureArmed = false;
    m_ReadbackPending = false;
    std::lock_guard<std::mutex> lock(m_ReadbackMutex);
    m_HasReadbackData = false;
}

void OceanRasterDepthCapture::ResolveReadyReadback()
{
    ReadbackMeta meta{};
    if (const void* mapped = m_Readback.MapNewestReady(m_Device, &meta))
    {
        std::lock_guard<std::mutex> lock(m_ReadbackMutex);
        const size_t count = size_t(meta.Resolution) * size_t(meta.Resolution);
        if (m_CPUWaterDepth.size() != count)
            m_CPUWaterDepth.resize(count, 60000.0f);
        std::memcpy(m_CPUWaterDepth.data(), mapped, sizeof(float32) * count);
        m_CPUReadbackResolution = meta.Resolution;
        m_ReadOriginX = meta.OriginX;
        m_ReadOriginZ = meta.OriginZ;
        m_ReadSizeX = meta.SizeX;
        m_ReadSizeZ = meta.SizeZ;
        m_ReadDeepWaterDepth = meta.Deep;
        m_HasReadbackData = true;
        m_Readback.Unmap(m_Device);
    }
}

bool OceanRasterDepthCapture::GetSampling(RG::RGFrame& frame, OceanRasterDepthSampling& out) const
{
    out = {};
    const auto* snapshot = m_PlannedFrame.IsFor(frame) && m_Plan ? &m_Plan->Proposed : m_Committed.get();
    if (!m_RenderServices || !snapshot->Generation || !m_Sampler.IsValid() ||
        snapshot->AvailableEpoch > m_RenderServices->Spine().WorldFrameEpoch())
        return false;
    frame.AllocatePassData<std::shared_ptr<OutputGeneration>>(snapshot->Generation);
    out.Texture = snapshot->Texture();
    out.Sampler = m_Sampler;
    out.Graph = frame.ImportExternalTexture("Ocean.RasterDepth.WaterDepth", out.Texture,
                                           ResourceState::ShaderResource, TextureFormat::R32_FLOAT);
    out.OriginX = snapshot->OriginX;
    out.OriginZ = snapshot->OriginZ;
    out.SizeX = snapshot->SizeX;
    out.SizeZ = snapshot->SizeZ;
    out.Deep = snapshot->Deep;
    return out.Graph.IsValid();
}

OceanRasterDepthCapture::FramePlan* OceanRasterDepthCapture::PlanSnapshot(
    RG::RGFrame& frame, const Snapshot& snapshot, uint64 epoch, bool convert)
{
    auto* plan = frame.AllocatePassData<FramePlan>();
    plan->Proposed = snapshot;
    plan->RetiredGeneration = m_Committed->Generation;
    plan->Owner.Stamp(frame);
    plan->Epoch = epoch;
    plan->Convert = convert;
    plan->Recorded = !convert;
    plan->Output = frame.ImportExternalTexture("Ocean.RasterDepth.WaterDepth", snapshot.Texture(),
                                              ResourceState::ShaderResource, TextureFormat::R32_FLOAT);
    m_Plan = plan;
    m_PlannedFrame.Stamp(frame);
    return plan;
}

bool OceanRasterDepthCapture::IsCurrentPlan(const FramePlan& plan) const
{
    return m_Plan == &plan && m_PlannedFrame.Frame == plan.Owner.Frame &&
           m_PlannedFrame.FrameIndex == plan.Owner.FrameIndex && m_ClaimedEpoch == plan.Epoch &&
           (plan.Convert ? m_OutputGeneration == plan.Proposed.Generation :
                           m_Committed->Generation == plan.Proposed.Generation &&
                           m_Committed->Serial == plan.Proposed.Serial);
}

void OceanRasterDepthCapture::ScheduleReadback(RG::RGFrame& frame, FramePlan& plan)
{
    frame.AddPass("Ocean.RasterDepth.Readback", PassPhase::kWorldRender,
        [this, packet = &plan](RG::RGPassBuilder& p)
        {
            p.Read(packet->Output, RG::RGTextureRead::CopySrc);
            p.PreventCulling();
            p.Prepare([this, packet](RG::RGPrepareContext& ctx) { return PrepareReadback(*packet, ctx); });
        },
        [this, packet = &plan](RG::RGContext& ctx) { RecordReadback(*packet, ctx); });
    frame.MarkOutput(plan.Output, RG::RGImageLayout::ShaderReadOnly);
}

bool OceanRasterDepthCapture::PrepareReadback(FramePlan& plan, RG::RGPrepareContext& ctx)
{
    if (!IsCurrentPlan(plan) || (plan.Recorded && !m_ReadbackPending))
    {
        plan.ReadbackOutput = {};
        plan.Reservation = 0;
        return true;
    }
    ResolveReadyReadback();
    if (!ctx.GetDevice() || !EnsureReadbackRing(*ctx.GetDevice(), plan.Proposed.Resolution))
        return false;
    if (!m_Readback.HasPendingWrite(*plan.Owner.Frame, plan.ReadbackOutput, plan.Reservation))
    {
        const auto& s = plan.Proposed;
        plan.ReadbackOutput = m_Readback.ReserveWrite(*plan.Owner.Frame,
            {s.OriginX, s.OriginZ, s.SizeX, s.SizeZ, s.Deep, s.Resolution}, plan.Reservation);
    }
    return true; // A busy slot is an optional copy, not a failed capture.
}

void OceanRasterDepthCapture::RecordReadback(const FramePlan& plan, RG::RGContext& ctx)
{
    if (!ctx.Cmd || !IsCurrentPlan(plan) ||
        !m_Readback.HasPendingWrite(*plan.Owner.Frame, plan.ReadbackOutput, plan.Reservation))
        return;
    const auto texture = ctx.GetTexture(plan.Output);
    if (texture != plan.Proposed.Texture())
        return;
    const uint32 resolution = plan.Proposed.Resolution;
    ctx.Cmd->CopyTextureToBuffer(texture, plan.ReadbackOutput, resolution, resolution, 0, 0, 0,
                                 sizeof(float32) * size_t(resolution));
    if (m_Readback.MarkRecorded(plan.ReadbackOutput, plan.Reservation) &&
        m_Committed->Generation == plan.Proposed.Generation && m_Committed->Serial == plan.Proposed.Serial)
        m_ReadbackPending = false;
}

void OceanRasterDepthCapture::OnFrameStreamRetired(const RG::RGFrame& frame)
{
    m_Readback.OnFrameStreamRetired(frame);
    if (m_PlannedFrame.Frame == &frame)
    {
        m_PlannedFrame = {};
        m_Plan = nullptr;
    }
}

bool OceanRasterDepthCapture::SampleDepth(float32 worldX, float32 worldZ,
                                          float32& outDepth) const
{
    std::lock_guard<std::mutex> lock(m_ReadbackMutex);
    if (!m_HasReadbackData || m_CPUReadbackResolution < 2u || m_CPUWaterDepth.empty())
        return false;

    const float32 sizeX = std::max(m_ReadSizeX, 1e-3f);
    const float32 sizeZ = std::max(m_ReadSizeZ, 1e-3f);
    const float32 u = (worldX - m_ReadOriginX) / sizeX;
    const float32 v = (worldZ - m_ReadOriginZ) / sizeZ;
    if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f)
        return false;

    const float32 halfTexel = 0.5f / float32(m_CPUReadbackResolution);
    const float32 sampleU = std::clamp(u, halfTexel, 1.0f - halfTexel);
    const float32 sampleV = std::clamp(v, halfTexel, 1.0f - halfTexel);
    const float32 gx = sampleU * float32(m_CPUReadbackResolution) - 0.5f;
    const float32 gz = sampleV * float32(m_CPUReadbackResolution) - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(gx)), 0,
                              static_cast<int>(m_CPUReadbackResolution) - 2);
    const int z0 = std::clamp(static_cast<int>(std::floor(gz)), 0,
                              static_cast<int>(m_CPUReadbackResolution) - 2);
    const float32 tx = gx - float32(x0);
    const float32 tz = gz - float32(z0);
    const uint32 n = m_CPUReadbackResolution;
    const float32 d00 = m_CPUWaterDepth[size_t(z0) * n + uint32(x0)];
    const float32 d10 = m_CPUWaterDepth[size_t(z0) * n + uint32(x0 + 1)];
    const float32 d01 = m_CPUWaterDepth[size_t(z0 + 1) * n + uint32(x0)];
    const float32 d11 = m_CPUWaterDepth[size_t(z0 + 1) * n + uint32(x0 + 1)];
    const float32 d0 = d00 + (d10 - d00) * tx;
    const float32 d1 = d01 + (d11 - d01) * tx;
    const float32 depth = d0 + (d1 - d0) * tz;
    const float32 deep = std::max(m_ReadDeepWaterDepth, 1.0f);
    if (depth >= deep - 1e-3f)
        return false;
    outDepth = std::clamp(depth, 0.0f, deep);
    return true;
}

void OceanRasterDepthCapture::EnsureView(Engine::Renderer::RenderServices& rs,
                                         const ViewDesc& mainView)
{
    if (m_ViewAllocated)
        return;
    m_RenderServices = &rs;
    m_CameraId = rs.Views().AllocateCamera("Ocean Raster Depth Camera");
    m_ViewId = rs.Views().AllocateView("Ocean Raster Depth", m_CameraId, ViewPurpose::UtilityCapture,
                               Rendering::ViewParticipation::OnDemand);
    rs.Views().SetViewActiveRenderPipeline(m_ViewId, false);
    rs.Views().SetViewRenderLayerMask(m_ViewId, mainView.renderLayerMask);
    rs.Views().SetViewWorldId(m_ViewId, mainView.worldId);
    m_ViewAllocated = true;
}

CameraData OceanRasterDepthCapture::MakeTopDownCamera(
    float32 centerX, float32 centerZ, float32 seaLevel,
    const OceanRasterDepthCaptureSettings& settings) const
{
    using GameEngine::Mathematics::MakeLookAtLH;
    using GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ;
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    const float32 sizeX = std::max(settings.SizeX, 1.0f);
    const float32 sizeZ = std::max(settings.SizeZ, 1.0f);
    const float32 topPadding = std::max(settings.TopPadding, 0.01f);
    const float32 deep = std::max(settings.DeepWaterDepth, 1.0f);

    const Vector3 eye(centerX, seaLevel + topPadding, centerZ);
    const Vector3 target(centerX, seaLevel + topPadding - 1.0f, centerZ);
    const Vector3 up(0.0f, 0.0f, 1.0f);
    const Matrix4x4 view = MakeLookAtLH(eye, target, up);
    const Matrix4x4 proj = MakeOrthographicLH_ZO_ReverseZ(
        -sizeX * 0.5f, sizeX * 0.5f, -sizeZ * 0.5f, sizeZ * 0.5f,
        topPadding, topPadding + deep);
    const Matrix4x4 viewProj = proj * view;

    CameraData out{};
    std::memcpy(out.view, view.Data(), sizeof(float) * 16u);
    std::memcpy(out.proj, proj.Data(), sizeof(float) * 16u);
    std::memcpy(out.viewProj, viewProj.Data(), sizeof(float) * 16u);
    out.cameraPos[0] = eye.x;
    out.cameraPos[1] = eye.y;
    out.cameraPos[2] = eye.z;
    return out;
}

bool OceanRasterDepthCapture::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                                             float32 centerX, float32 centerZ,
                                             float32 seaLevel,
                                             const OceanRasterDepthCaptureSettings& settings)
{
    if (!settings.Enabled)
        return false;

    auto* device = d.Services.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    const uint64 epoch = d.Services.Spine().WorldFrameEpoch();
    ResolveReadyReadback();
    if (m_PlannedFrame.IsFor(d.Frame) || m_ClaimedEpoch == epoch)
        return true;
    if (!EnsurePipeline(*device))
        return false;
    Snapshot proposed{};
    proposed.Resolution = std::clamp(settings.Resolution == 0u ? 512u : settings.Resolution, 16u, 4096u);
    proposed.Mask = settings.RenderLayerMask & d.View.renderLayerMask;
    if (proposed.Mask == 0u)
        return false;
    proposed.SizeX = std::max(settings.SizeX, 1.0f);
    proposed.SizeZ = std::max(settings.SizeZ, 1.0f);
    const float32 snappedCenterX = SnapToTexel(centerX, proposed.SizeX / proposed.Resolution);
    const float32 snappedCenterZ = SnapToTexel(centerZ, proposed.SizeZ / proposed.Resolution);
    proposed.OriginX = snappedCenterX - proposed.SizeX * 0.5f;
    proposed.OriginZ = snappedCenterZ - proposed.SizeZ * 0.5f;
    proposed.Deep = std::max(settings.DeepWaterDepth, 1.0f);
    proposed.TopPadding = std::max(settings.TopPadding, 0.01f);
    proposed.SeaLevel = seaLevel;
    proposed.World = d.View.worldId;
    m_ClaimedEpoch = epoch;
    const bool dirty = !m_Committed->Matches(proposed);
    if (!dirty)
    {
        if (m_ReadbackPending)
            ScheduleReadback(d.Frame, *PlanSnapshot(d.Frame, *m_Committed, epoch, false));
        return true;
    }
    EnsureView(d.Services, d.View);
    if (m_ViewId == 0 || m_CameraId == 0)
        return false;
    d.Services.Views().SetCameraData(m_CameraId,
        MakeTopDownCamera(snappedCenterX, snappedCenterZ, seaLevel, settings));
    d.Services.Views().SetViewRenderLayerMask(m_ViewId, proposed.Mask);
    d.Services.Views().SetViewWorldId(m_ViewId, proposed.World);
    // OnDemand extraction and requests advance on the global world clock.
    if (!m_CaptureArmed || epoch != m_ArmedEpoch + 1u)
    {
        d.Services.Views().RequestViewFrame(m_ViewId);
        m_CaptureArmed = true;
        m_ArmedEpoch = epoch;
        if (m_ReadbackPending)
            ScheduleReadback(d.Frame, *PlanSnapshot(d.Frame, *m_Committed, epoch, false));
        return true;
    }
    m_CaptureArmed = false;
    const uint32 resolution = proposed.Resolution;
    if (!EnsureOutput(*device, resolution))
        return false;
    proposed.Generation = m_OutputGeneration;
    proposed.Serial = m_CaptureSerial;
    proposed.AvailableEpoch = m_Committed->Generation == proposed.Generation &&
                              m_Committed->AvailableEpoch <= epoch ? epoch : epoch + 1u;
    d.Services.WriteViewLightBuffer(m_ViewId);
    d.Services.BuildWorldBatchKeysForView(m_ViewId);
    if (!d.Services.ScheduleBucketerDispatchesForView(d.Frame, m_ViewId))
        return false;

    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    d.Services.Views().SetViewClearConfig(m_ViewId, clear);

    TextureDesc colorDesc{};
    colorDesc.width = resolution;
    colorDesc.height = resolution;
    colorDesc.depth = 1;
    colorDesc.mipLevels = 1;
    colorDesc.arrayLayers = 1;
    colorDesc.sampleCount = 1;
    colorDesc.format = static_cast<uint32>(TextureFormat::RGBA8_UNORM);
    colorDesc.usage = static_cast<uint32>(TextureUsage::RenderTarget);
    colorDesc.debugName = "Ocean.RasterDepth.Color";

    TextureDesc depthDesc{};
    depthDesc.width = resolution;
    depthDesc.height = resolution;
    depthDesc.depth = 1;
    depthDesc.mipLevels = 1;
    depthDesc.arrayLayers = 1;
    depthDesc.sampleCount = 1;
    depthDesc.format = static_cast<uint32>(d.Services.GetDepthFormat());
    depthDesc.usage =
        static_cast<uint32>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    depthDesc.debugName = "Ocean.RasterDepth.Depth";

    // The hidden world pass resolves scene targets through RenderServices' physical
    // resource path, so these must be imported from the persistent pool.
    const RG::RGTexture color =
        d.Frame.ImportPersistentTexture("Ocean.RasterDepth.Color", colorDesc);
    const RG::RGTexture depth =
        d.Frame.ImportPersistentTexture("Ocean.RasterDepth.Depth", depthDesc);
    const RG::RGTexture output = d.Frame.ImportExternalTexture(
        "Ocean.RasterDepth.WaterDepth", proposed.Texture(), ResourceState::ShaderResource,
        TextureFormat::R32_FLOAT);
    if (!color.IsValid() || !depth.IsValid() || !output.IsValid())
        return false;

    Engine::Renderer::RenderServices::WorldPassTargetsRG targets{};
    targets.Color = color;
    targets.Depth = depth;
    d.Services.EmitProducerForwardCommandsForView(m_ViewId);
    const auto world = d.Services.AddWorldPassForView(d.Frame, m_ViewId, targets,
                                                      MaterialKeyword::Instanced);
    if (!world.Pass.IsValid())
        return false;

    // Converter params live in the frame's upload ring (host-coherent, written
    // here at declare time); the pass captures only {buffer, offset}.
    const auto paramsAlloc = d.Frame.AllocUpload<OceanRasterDepthParamsGPU>();
    if (!paramsAlloc.Valid())
        return false;
    OceanRasterDepthParamsGPU params{};
    params.NearFarDeepResolution[0] = proposed.TopPadding;
    params.NearFarDeepResolution[1] = proposed.TopPadding + proposed.Deep;
    params.NearFarDeepResolution[2] = proposed.Deep;
    params.NearFarDeepResolution[3] = static_cast<float32>(resolution);
    *paramsAlloc.Ptr = params;

    auto* plan = PlanSnapshot(d.Frame, proposed, epoch, true);
    plan->Depth = depth;
    plan->Params = paramsAlloc.Buffer;
    plan->ParamsOffset = paramsAlloc.Offset;
    d.Frame.AddPass("Ocean.RasterDepth.Convert", PassPhase::kWorldRender,
        [this, plan](RG::RGPassBuilder& p)
        {
            p.UsePipeline(m_PipelineId);
            p.Read(plan->Depth, RG::RGTextureRead::SampledCompute);
            p.Write(plan->Output, RG::RGTextureWrite::Storage);
            p.Prepare([this, plan](RG::RGPrepareContext& ctx) { return PrepareConverter(*plan, ctx); });
        },
        [this, plan](RG::RGContext& ctx) { RecordConverter(*plan, ctx); });
    ScheduleReadback(d.Frame, *plan);
    return true;
}

bool OceanRasterDepthCapture::PrepareConverter(FramePlan& plan, RG::RGPrepareContext& ctx) const
{
    plan.Commands = {};
    auto* device = ctx.GetDevice();
    if (!device || plan.Recorded || !IsCurrentPlan(plan) || plan.Proposed.Serial != m_CaptureSerial)
        return false;
    const auto pipeline = ctx.PreparePipeline(m_PipelineId);
    const auto depth = ctx.GetTexture(plan.Depth);
    const auto output = ctx.GetTexture(plan.Output);
    if (!pipeline || !depth.IsValid() || output != plan.Proposed.Texture())
        return false;
    DescriptorSetDesc desc{};
    desc.layout = m_Layout;
    desc.debugName = "Ocean.RasterDepth.Convert.DS";
    desc.transient = true;
    const auto set = device->CreateDescriptorSet(desc);
    if (!set.IsValid())
        return false;
    device->UpdateCombinedImageSamplerBinding(set, 0, depth, m_Sampler);
    device->UpdateStorageImageBinding(set, 1, output);
    device->UpdateBufferBinding(set, 2, plan.Params, plan.ParamsOffset, sizeof(OceanRasterDepthParamsGPU));
    const uint32 groups = (plan.Proposed.Resolution + 7u) / 8u;
    plan.Commands = {pipeline, set, groups, groups, 1u, output};
    return true;
}

void OceanRasterDepthCapture::RecordConverter(FramePlan& plan, RG::RGContext& ctx)
{
    if (plan.Recorded || !IsCurrentPlan(plan) || plan.Proposed.Serial != m_CaptureSerial ||
        plan.Commands.Target != plan.Proposed.Texture())
        return;
    if (!RecordOceanPreparedDispatch(ctx.Cmd, plan.Commands))
        return;
    *m_Committed = plan.Proposed;
    ++m_CaptureSerial;
    plan.Recorded = true;
    m_ReadbackPending = true;
}

} // namespace GameEngine::Ocean
