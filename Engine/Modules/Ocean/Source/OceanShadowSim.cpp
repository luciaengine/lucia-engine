#include "Ocean/OceanShadowSim.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Ocean/OceanFrameStamp.h"
#include "Ocean/OceanPreparedDispatch.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include <algorithm>
#include <cmath>

namespace GameEngine::Ocean
{
using namespace Rendering;
namespace
{
struct alignas(16) ShadowInputGPU
{
    float Rect[4]{};
    float Values[4]{};
};
struct alignas(16) ShadowParamsGPU
{
    OceanCascadeLayoutGPU Current{}, Previous{};
    float Simulation[4]{}; // sea level, previous weight, soft jitter diameter, hard jitter diameter
    float Frame[4]{};      // history valid, seed, input count, pad
    ShadowInputGPU Inputs[64]{};
};
static_assert(sizeof(ShadowParamsGPU) == 2336);
} // namespace
struct OceanShadowSim::History
{
    OceanCascadeArray Fields[2];
    uint32 Index = 0;
    RenderGraph::RGFrameStamp RecordedFrame, PlannedFrame;
    FramePlan* Plan = nullptr;
    uint64 ClaimedEpoch = ~0ull;
    std::chrono::steady_clock::time_point LastUsed{};
    float Time = 0, Sun[3]{};
    bool Valid = false;
};
struct OceanShadowSim::FramePlan
{
    // Keep both arrays alive if a later declaration prunes or replaces the map entry.
    std::shared_ptr<History> Lease;
    RenderGraph::RGFrameStamp Owner;
    uint32 WriteIndex = 0;
    OceanCascadeLayoutGPU Layout{};
    float Time = 0, Sun[3]{}, Channels[2]{};
    TextureHandle ExpectedOutput{};
    RenderGraph::RGTexture Previous{}, Output{}, ShadowArray{};
    Engine::Renderer::Pipeline::PipelineBufferBindingRG ShadowData{};
    BufferHandle Params{};
    uint64 ParamsOffset = 0;
    SamplerHandle ShadowSampler{};
    OceanPreparedDispatch Commands;
    bool Recorded = false;
};
OceanShadowSim::OceanShadowSim() = default;
OceanShadowSim::~OceanShadowSim()
{
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}
bool OceanShadowSim::Initialize(IDevice *device)
{
    if (m_Pipeline.IsValid())
        return true;
    // A program that failed to load stays failed: logged once, no retry per frame.
    if (m_LoadFailed || !device)
        return false;
    m_LoadFailed = true;
    m_Device = device;
    const auto dir = OceanShaderDirectory("ocean_shadow_sim.comp");
    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_shadow_sim";
    req.baseDirectory = dir;
    req.cacheRoot = ".Cache/Shaders";
    req.includeDirs = {dir.parent_path()};
    req.stages = {{"cs", "ocean_shadow_sim.comp", "main", {}}};
    ShaderProgramCompileResult result{};
    std::string error;
    const auto stage = LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &error)
                           ? result.stageBytes.find("cs")
                           : result.stageBytes.end();
    if (stage == result.stageBytes.end() || stage->second.empty())
    {
        Logger::Log::Error("Ocean shadows: ocean_shadow_sim failed to load, so water shadows stay off until "
                           "restart: {}",
                           error);
        return false;
    }
    m_Layout.debugName = "Ocean.Shadows.Set0";
    m_Layout.bindings = {{0, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                         {1, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute},
                         {2, DescriptorType::StorageImage, 1, kShaderStageCompute},
                         {8, DescriptorType::UniformBuffer, 1, kShaderStageCompute},
                         {9, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute}};
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    ComputePipelineDesc desc{};
    desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));
    desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(m_Layout));
    desc.DebugName = "Ocean.Shadows";
    m_Pipeline = device->InternComputePipeline(desc);
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean.Shadows.Sampler"));
    m_LoadFailed = !m_Pipeline.IsValid() || !m_Sampler.IsValid();
    return !m_LoadFailed;
}
void OceanShadowSim::RebaseOrigin(float x, float z)
{
    for (auto &[view, h] : m_Histories)
        for (auto &field : h->Fields)
            field.RebaseOrigin(x, z);
}
bool OceanShadowSim::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare &d, const OceanParamsGPU &params)
{
    const uint64 frame = d.Frame.FrameIndex();
    if (!m_Settings.Enabled)
    {
        m_Histories.Clear();
        return false;
    }
    const auto data = d.ResolveBuffer(Engine::Renderer::Pipeline::Names::Res::ShadowData);
    const auto array = d.Services.GetShadowMapArrayRG(d.Frame, d.View.id);
    const auto *resources = d.Services.Views().GetViewShadowResources(d.View.id);
    const auto *camera = d.Services.Views().FindCameraData(d.View.cameraId);
    if (!data.IsValid() || !array.IsValid() || !resources || !resources->shadowSampler.IsValid() || !camera ||
        !Initialize(d.Services.GetDevice()))
        return false;
    const uint32 resolution = std::clamp(m_Settings.Resolution, 32u, 1024u);
    const uint32 count = std::clamp(m_Settings.CascadeCount, 1u, kMaxOceanLodCascades);
    const auto now = std::chrono::steady_clock::now();
    m_Histories.EraseIf([&](ViewId view, const auto& entry) {
        return view != d.View.id && entry && now - entry->LastUsed > kOceanViewHistoryIdleTimeout;
    });
    auto &slot = m_Histories.GetOrInsert(d.View.id);
    if (!slot || slot->Fields[0].GetResolution() != resolution || slot->Fields[0].GetLodCount() != count)
    {
        slot = std::make_shared<History>();
        const std::string prefix = "Ocean.Shadows.View" + std::to_string(d.View.id);
        for (uint32 i = 0; i < 2; ++i)
            if (!slot->Fields[i].Initialize(m_Device, resolution, count, TextureFormat::R16G16_FLOAT, 64.0f,
                                            (prefix + (i ? ".B" : ".A")).c_str()))
            {
                m_Histories.Erase(d.View.id);
                return false;
            }
    }
    auto &h = *slot;
    h.LastUsed = now;
    if (h.PlannedFrame.IsFor(d.Frame))
        return true;
    const uint64 epoch = d.Services.Spine().WorldFrameEpoch();
    if (h.ClaimedEpoch == epoch)
        return false;
    const float elapsed = params.Time - h.Time;
    float sunChange = 0;
    for (uint32 i = 0; i < 3; ++i)
        sunChange += std::abs(h.Sun[i] - params.SunDirection[i]);
    const bool history =
        h.Valid && elapsed >= 0 && elapsed < 1.0f && sunChange < 0.02f &&
        h.RecordedFrame.Frame == &d.Frame && h.RecordedFrame.FrameIndex + 1u == frame;
    const uint32 write = h.Index ^ 1u;
    const auto projected = h.Fields[write].ProjectedLayout(camera->cameraPos[0], camera->cameraPos[2]);
    const auto upload = d.Frame.AllocUpload<ShadowParamsGPU>();
    if (!upload.Valid())
        return false;
    ShadowParamsGPU u{};
    u.Current = projected;
    u.Previous = h.Fields[h.Index].GetLayout();
    u.Simulation[0] = params.SeaLevel;
    u.Simulation[1] =
        history ? std::pow(std::clamp(m_Settings.TemporalWeight, 0.0f, 0.9999f),
                           std::max(elapsed, 1.0f / 120.0f) * std::max(m_Settings.SimulationFrequency, 1.0f))
                : 0.0f;
    u.Simulation[2] = std::max(m_Settings.JitterDiameter, 0.0f);
    u.Simulation[3] = std::max(m_Settings.HardJitterDiameter, 0.0f);
    u.Frame[0] = history ? 1.0f : 0.0f;
    u.Frame[1] = static_cast<float>(frame % 4096u);
    uint32 inputCount = 0;
    for (const auto &input : m_Inputs)
    {
        if (input.Family != OceanInputFamily::Shadow || inputCount == 64u)
            continue;
        auto &target = u.Inputs[inputCount++];
        target.Rect[0] = input.CenterX;
        target.Rect[1] = input.CenterZ;
        target.Rect[2] = input.ExtentX;
        target.Rect[3] = input.ExtentZ;
        target.Values[0] = input.Value[0];
        target.Values[1] = input.Value[1];
        target.Values[2] = input.Feather;
        target.Values[3] = static_cast<float>(input.Blend);
    }
    u.Frame[2] = static_cast<float>(inputCount);
    *upload.Ptr = u;
    auto* plan = d.Frame.AllocatePassData<FramePlan>();
    plan->Lease = slot;
    plan->Owner.Stamp(d.Frame);
    plan->WriteIndex = write;
    plan->Layout = projected;
    plan->Time = params.Time;
    std::copy_n(params.SunDirection, 3, plan->Sun);
    plan->Channels[0] = m_Settings.HardChannelScale;
    plan->Channels[1] = m_Settings.SoftChannelScale;
    plan->ExpectedOutput = h.Fields[write].GetTexture();
    plan->Previous = h.Fields[h.Index].ImportRG(d.Frame);
    plan->Output = h.Fields[write].ImportRG(d.Frame);
    plan->ShadowArray = array;
    plan->ShadowData = data;
    plan->Params = upload.Buffer;
    plan->ParamsOffset = upload.Offset;
    plan->ShadowSampler = resources->shadowSampler;
    d.Frame.AddPass(
        "OceanShadowAccumulate", PassPhase::kEarlySetup,
        [this, plan, view = d.View.id](RenderGraph::RGPassBuilder &p) {
            p.UsePipeline(m_Pipeline);
            p.Read(plan->ShadowArray, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(plan->Previous, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(plan->Output, RenderGraph::RGTextureWrite::Storage);
            if (plan->ShadowData.Graph.IsValid())
                p.Read(plan->ShadowData.Graph, RenderGraph::RGBufferRead::Uniform);
            p.Prepare([this, plan, view](RenderGraph::RGPrepareContext& ctx) { return Prepare(view, *plan, ctx); });
        },
        [this, plan, view = d.View.id](RenderGraph::RGContext &ctx) { Record(view, *plan, ctx); });
    d.Frame.MarkOutput(plan->Output, RenderGraph::RGImageLayout::ShaderReadOnly);
    h.PlannedFrame.Stamp(d.Frame);
    h.Plan = plan;
    h.ClaimedEpoch = epoch;
    return true;
}

bool OceanShadowSim::IsCurrentPlan(ViewId view, const FramePlan& plan) const
{
    const auto* slot = m_Histories.Find(view);
    if (!slot || !*slot || *slot != plan.Lease || plan.Recorded)
        return false;
    const auto& h = **slot;
    return h.Plan == &plan && h.PlannedFrame.Frame == plan.Owner.Frame &&
        h.PlannedFrame.FrameIndex == plan.Owner.FrameIndex &&
        h.Fields[plan.WriteIndex].GetTexture() == plan.ExpectedOutput;
}

bool OceanShadowSim::Prepare(ViewId view, FramePlan& plan, RenderGraph::RGPrepareContext& ctx) const
{
    plan.Commands = {};
    if (!IsCurrentPlan(view, plan))
        return false;
    auto* device = ctx.GetDevice();
    if (!device)
        return false;
    const auto pipeline = ctx.PreparePipeline(m_Pipeline);
    const auto previous = ctx.GetTexture(plan.Previous);
    const auto output = ctx.GetTexture(plan.Output);
    const auto array = ctx.GetTexture(plan.ShadowArray);
    const auto& data = plan.ShadowData;
    const auto dataBuffer = data.Graph.IsValid() ? ctx.GetBuffer(data.Graph) : data.Buffer;
    if (!pipeline || !previous.IsValid() || output != plan.ExpectedOutput || !array.IsValid() ||
        !dataBuffer.IsValid() || !plan.Params.IsValid())
        return false;
    DescriptorSetDesc desc{};
    desc.layout = m_Layout;
    desc.transient = true;
    desc.debugName = "Ocean.Shadows.DS";
    const auto set = device->CreateDescriptorSet(desc);
    if (!set.IsValid())
        return false;
    device->UpdateBufferBinding(set, 0, plan.Params, plan.ParamsOffset, sizeof(ShadowParamsGPU));
    device->UpdateCombinedImageSamplerBinding(set, 1, previous, m_Sampler);
    device->UpdateStorageImageBinding(set, 2, output);
    device->UpdateBufferBinding(set, 8, dataBuffer, data.Offset, data.Size);
    device->UpdateCombinedImageSamplerBinding(set, 9, array, plan.ShadowSampler);
    const auto& field = plan.Lease->Fields[plan.WriteIndex];
    const uint32 groups = (field.GetResolution() + 7u) / 8u;
    plan.Commands = {pipeline, set, groups, groups, field.GetLodCount(), output};
    return true;
}

void OceanShadowSim::Record(ViewId view, FramePlan& plan, RenderGraph::RGContext& ctx)
{
    if (!IsCurrentPlan(view, plan) || plan.Commands.Target != plan.ExpectedOutput ||
        !RecordOceanPreparedDispatch(ctx.Cmd, plan.Commands))
        return;
    plan.Recorded = true;
    auto& h = *plan.Lease;
    h.Fields[plan.WriteIndex].CommitLayout(plan.Layout);
    h.Index = plan.WriteIndex;
    h.RecordedFrame = plan.Owner;
    h.Time = plan.Time;
    h.Valid = true;
    std::copy_n(plan.Sun, 3, h.Sun);
}

const OceanShadowSim::FramePlan* OceanShadowSim::FindPlan(ViewId view, const RenderGraph::RGFrame& frame) const
{
    const auto* slot = m_Histories.Find(view);
    return slot && *slot && (*slot)->PlannedFrame.IsFor(frame) ? (*slot)->Plan : nullptr;
}

bool OceanShadowSim::FillSampling(ViewId view, const RenderGraph::RGFrame& frame,
                                  OceanShadowSamplingGPU& out) const
{
    out = {};
    const auto* plan = FindPlan(view, frame);
    if (!plan)
        return false;
    out.Layout = plan->Layout;
    out.Channels[0] = plan->Channels[0];
    out.Channels[1] = plan->Channels[1];
    out.Channels[2] = 1.0f;
    return true;
}

TextureHandle OceanShadowSim::GetTexture(ViewId view) const
{
    const auto* slot = m_Histories.Find(view);
    return slot && *slot && (*slot)->Valid ? (*slot)->Fields[(*slot)->Index].GetTexture() : TextureHandle{};
}

TextureHandle OceanShadowSim::GetTexture(ViewId view, const RenderGraph::RGFrame& frame) const
{
    const auto* plan = FindPlan(view, frame);
    return plan ? plan->ExpectedOutput : GetTexture(view);
}

RenderGraph::RGTexture OceanShadowSim::ImportRG(RenderGraph::RGFrame& frame, ViewId view) const
{
    const auto* plan = FindPlan(view, frame);
    const auto* slot = m_Histories.Find(view);
    if (plan)
        return plan->Lease->Fields[plan->WriteIndex].ImportRG(frame);
    return slot && *slot && (*slot)->Valid ? (*slot)->Fields[(*slot)->Index].ImportRG(frame)
                                         : RenderGraph::RGTexture{};
}

void OceanShadowSim::ForgetFrame(const RenderGraph::RGFrame& frame)
{
    for (auto& [view, h] : m_Histories)
    {
        if (h->PlannedFrame.Frame == &frame)
        {
            h->PlannedFrame = {};
            h->Plan = nullptr;
        }
        if (h->RecordedFrame.Frame == &frame)
            h->RecordedFrame = {};
    }
}
} // namespace GameEngine::Ocean
