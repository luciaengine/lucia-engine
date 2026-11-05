#include "Ocean/OceanDynWaves.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
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

namespace
{

} // anonymous namespace

bool OceanDynWaves::Initialize(IDevice* device)
{
    // The cascade bakes into a narrow storage format, which this backend's
    // storage-image list does not include; the texture would be refused at
    // creation and its invalid handle would poison every submit that binds
    // it. Declining here keeps the surface: the caller already degrades.
    if (device && !device->GetCapabilities().supportsNarrowStorageFormats)
        return false;
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    // RG16F (height, velocity) cascade — ping-pong pair.
    if (!m_State[0].Initialize(device, kDynResolution, kDynLodCount, TextureFormat::R16G16_FLOAT,
                               kDynBaseScale, "Ocean_DynWaves_A") ||
        !m_State[1].Initialize(device, kDynResolution, kDynLodCount, TextureFormat::R16G16_FLOAT,
                               kDynBaseScale, "Ocean_DynWaves_B"))
    {
        Logger::Log::Warning("OceanDynWaves: cascade allocation failed");
        return false;
    }
    m_Resolution = m_State[0].GetResolution();
    m_LodCount = m_State[0].GetLodCount();

    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_DynWaves_Sampler"));
    if (!m_Sampler.IsValid())
        return false;

    const fs::path shaderDir = OceanShaderDirectory("ocean_dynwaves_sim.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanDynWaves: ocean_dynwaves_sim.comp not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    ShaderProgramCompileRequest req{};
    req.debugName = "ocean_dynwaves_sim";
    req.baseDirectory = shaderDir;
    req.cacheRoot = cacheRoot;
    req.includeDirs = {includeDir};
    req.stages = {{"cs", "ocean_dynwaves_sim.comp", "main", {}}};

    ShaderProgramCompileResult result{};
    std::string err;
    if (!LoadOceanShaderProgram(req, device->PreferredShaderSource(), result, &err))
    {
        Logger::Log::Error("OceanDynWaves: compile failed: {}", err);
        return false;
    }
    auto it = result.stageBytes.find("cs");
    if (it == result.stageBytes.end() || it->second.empty())
    {
        Logger::Log::Error("OceanDynWaves: no SPIR-V");
        return false;
    }
    auto shaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));

    // Layout: UBO(0), prev-state sampler(1), output-state storage image(2),
    // seabed-depth sampler(3) for shallow-water attenuation.
    m_Layout = DescriptorSetLayoutDesc{};
    m_Layout.debugName = "OceanDynWaves_Set0";
    auto addBinding = [&](uint32 binding, DescriptorType type) {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = type;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        m_Layout.bindings.push_back(b);
    };
    addBinding(0, DescriptorType::UniformBuffer);
    addBinding(1, DescriptorType::CombinedImageSampler);
    addBinding(2, DescriptorType::StorageImage);
    addBinding(3, DescriptorType::CombinedImageSampler);

    ComputePipelineDesc cd{};
    cd.ComputeShader = shaderBytes;
    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
    cd.DebugName = "OceanDynWaves";
    m_Pipe = m_Device->InternComputePipeline(cd);
    if (!m_Pipe.IsValid())
        return false;

    m_Ready = true;
    Logger::Log::Info("OceanDynWaves: ready ({}x{} x{} LODs)", m_Resolution, m_Resolution, m_LodCount);
    return true;
}

TextureHandle OceanDynWaves::GetTexture() const
{
    return m_State[m_WriteIndex].GetTexture();
}

const OceanCascadeLayoutGPU& OceanDynWaves::GetLayout() const
{
    return m_State[m_WriteIndex].GetLayout();
}

TextureHandle OceanDynWaves::GetTexture(const RenderGraph::RGFrame& frame) const
{
    return m_PlannedFrame.IsFor(frame) && !m_Plan->Steps.empty()
        ? m_State[m_Plan->Steps.back().WriteIndex].GetTexture() : GetTexture();
}

const OceanCascadeLayoutGPU& OceanDynWaves::GetLayout(const RenderGraph::RGFrame& frame) const
{
    return m_PlannedFrame.IsFor(frame) && !m_Plan->Steps.empty()
        ? m_Plan->Steps.back().Cascade : GetLayout();
}

RenderGraph::RGTexture OceanDynWaves::ImportWriteRG(RenderGraph::RGFrame& frame) const
{
    const uint32 index = m_PlannedFrame.IsFor(frame) && !m_Plan->Steps.empty()
        ? m_Plan->Steps.back().WriteIndex : m_WriteIndex;
    return m_State[index].ImportRG(frame);
}

RenderGraph::RGTexture OceanDynWaves::ImportWriteRG(RenderGraph::RGFrame& frame,
                                                    uint32 writeIndex) const
{
    return m_State[writeIndex & 1u].ImportRG(frame);
}

RenderGraph::RGTexture OceanDynWaves::ImportPrevRG(RenderGraph::RGFrame& frame,
                                                   uint32 writeIndex) const
{
    return m_State[(writeIndex & 1u) ^ 1u].ImportRG(frame);
}

const OceanCascadeFramePlan& OceanDynWaves::PlanSubsteps(
    RenderGraph::RGFrame& frame, float cameraX, float cameraZ)
{
    if (!m_PlannedFrame.IsFor(frame))
    {
        m_Plan = PlanOceanCascadeSteps(frame, m_State, m_WriteIndex, m_PendingSubsteps, cameraX, cameraZ);
        m_PlannedFrame.Stamp(frame);
        m_NextSubstep = 0;
    }
    return *m_Plan;
}

void OceanDynWaves::ConfigureSimulation(float32 frequency, uint32 maximumSubsteps,
                                        float32 damping, float32 courantNumber,
                                        float32 gravity, float32 shallowAttenuation,
                                        float32 horizontalDisplacement,
                                        float32 displacementClamp, uint32 minimumCascade,
                                        uint32 maximumCascade)
{
    m_UseFixedStep = true;
    m_SimulationFrequency = std::max(frequency, 1.0f);
    m_MaximumSubsteps = std::clamp(maximumSubsteps, 1u, 16u);
    m_Params.Damping = std::max(damping, 0.0f);
    m_Params.CourantNumber = std::clamp(courantNumber, 0.01f, 1.0f);
    m_Params.Gravity = std::max(gravity, 0.01f);
    m_Params.ShallowAttenuation = std::clamp(shallowAttenuation, 0.0f, 1.0f);
    m_Params.HorizontalDisplacement = std::max(horizontalDisplacement, 0.0f);
    m_Params.DisplacementClamp = std::max(displacementClamp, 0.01f);
    m_Params.MinimumCascade = std::min(minimumCascade, kMaxOceanLodCascades - 1u);
    m_Params.MaximumCascade = std::clamp(maximumCascade, m_Params.MinimumCascade,
                                         kMaxOceanLodCascades - 1u);
}

void OceanDynWaves::ClearSimulationConfiguration()
{
    m_UseFixedStep = false;
    m_TimeAccumulator = 0.0f;
    m_Params.CourantNumber = 0.7f;
    m_Params.Gravity = 9.81f;
    m_Params.ShallowAttenuation = 1.0f;
    m_Params.HorizontalDisplacement = 0.0f;
    m_Params.DisplacementClamp = 4.0f;
    m_Params.MinimumCascade = 0u;
    m_Params.MaximumCascade = kMaxOceanLodCascades - 1u;
}

void OceanDynWaves::SetShallowWaterInputs(TextureHandle seabedDepth,
                                          SamplerHandle seabedSampler, bool available)
{
    m_SeabedDepth = seabedDepth;
    m_SeabedSampler = seabedSampler;
    m_Params.SeabedDepthAvailable =
        available && seabedDepth.IsValid() && seabedSampler.IsValid() ? 1u : 0u;
}

void OceanDynWaves::SetFrameInputs(const OceanWaveImpulseGPU* impulses, uint32 count,
                                   float waveSpeed, float damping, float deltaTime)
{
    if (IsQuiescent())
    {
        m_TimeAccumulator = 0.0f;
        m_PendingSubsteps = 0u;
        m_SubstepDelta = 0.0f;
        m_PendingImpulseCount = 0u;
        m_Params.ImpulseCount = 0u;
    }
    const float32 pendingSeconds = static_cast<float32>(m_PendingSubsteps) * m_SubstepDelta;
    const uint32 n = std::min(count, kMaxOceanWaveImpulses);
    if (n > 0)
        m_SimulatedSecondsSinceImpulse = 0.0f;
    else if (deltaTime > 0.0f && m_SimulatedSecondsSinceImpulse < kQuiescentSeconds)
        m_SimulatedSecondsSinceImpulse =
            std::min(kQuiescentSeconds, m_SimulatedSecondsSinceImpulse + deltaTime);
    if (impulses && n > 0)
    {
        const uint32 appended = std::min(n, kMaxOceanWaveImpulses - m_PendingImpulseCount);
        std::memcpy(m_Params.Impulses + m_PendingImpulseCount, impulses,
                    static_cast<size_t>(appended) * sizeof(OceanWaveImpulseGPU));
        m_PendingImpulseCount += appended;
    }
    m_Params.WaveSpeed = waveSpeed;
    if (!m_UseFixedStep)
        m_Params.Damping = damping;

    if (m_UseFixedStep)
    {
        if (IsQuiescent() && n == 0u)
            m_TimeAccumulator = 0.0f;
        else
            m_TimeAccumulator += pendingSeconds + std::max(deltaTime, 0.0f);
        const float32 fixedDelta = 1.0f / m_SimulationFrequency;
        const uint32 available = static_cast<uint32>(
            std::floor((m_TimeAccumulator + 1e-6f) / fixedDelta));
        m_PendingSubsteps = std::min(available, m_MaximumSubsteps);
        m_SubstepDelta = fixedDelta;
        m_TimeAccumulator = std::max(
            0.0f, m_TimeAccumulator - static_cast<float32>(m_PendingSubsteps) * fixedDelta);
        // Avoid an unbounded catch-up tail after a hitch or debugger pause.
        m_TimeAccumulator = std::min(m_TimeAccumulator,
                                     fixedDelta * static_cast<float32>(m_MaximumSubsteps));
    }
    else
    {
        m_SubstepDelta = pendingSeconds + std::max(deltaTime, 0.0f);
        m_PendingSubsteps = (m_SubstepDelta > 0.0f || m_PendingImpulseCount > 0u) ? 1u : 0u;
    }
}

void OceanDynWaves::FillParams(OceanDynWavesParamsGPU& out, const OceanCascadeStep& step,
                               uint32 substepIndex) const
{
    out = m_Params;
    out.Resolution = m_Resolution;
    out.LodCount = m_LodCount;
    out.DeltaTime = m_SubstepDelta;
    out.ImpulseCount = substepIndex == 0u ? m_PendingImpulseCount : 0u;
    out.ResetHistory = m_ResetHistory && substepIndex == 0u ? 1u : 0u;
    out.Cascade = step.Cascade;
    out.PrevCascade = step.PrevCascade;
}

void OceanDynWaves::FinishSubsteps()
{
    m_PendingSubsteps = 0u;
    m_PendingImpulseCount = 0u;
    m_Params.ImpulseCount = 0u;
    m_ResetHistory = false;
}

OceanPreparedSimulationDispatch OceanDynWaves::PrepareDispatch(
    IDevice* device, BufferHandle paramsBuffer, uint64 paramsOffset,
    const OceanCascadeFramePlan& plan, uint32 substepIndex) const
{
    // Every step prepares before the first step records. Only consumed steps are refused here.
    if (m_Plan != &plan || substepIndex >= plan.Steps.size() || substepIndex < m_NextSubstep)
        return {};
    const auto& step = plan.Steps[substepIndex];
    if (step.WriteIndex >= 2u ||
        m_State[0].GetTexture() != plan.Textures[0] ||
        m_State[1].GetTexture() != plan.Textures[1])
        return {};
    if (!m_Ready || !device || !paramsBuffer.IsValid())
        return {};

    const uint32 writeIndex = step.WriteIndex;
    const uint32 readIndex = writeIndex ^ 1u;
    const TextureHandle prevState = m_State[readIndex].GetTexture();
    const TextureHandle nextState = m_State[writeIndex].GetTexture();
    const uint32 layers = m_LodCount;

    PipelineHandle pipe = device->TryGetWarmComputePipeline(m_Pipe);
    if (!pipe)
        return {};

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = m_Layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanDynWaves_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);
    if (!ds.IsValid())
        return {};

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanDynWavesParamsGPU));
    device->UpdateCombinedImageSamplerBinding(ds, 1, prevState, m_Sampler);
    device->UpdateStorageImageBinding(ds, 2, nextState);
    const TextureHandle seabed = m_SeabedDepth.IsValid() ? m_SeabedDepth : prevState;
    const SamplerHandle seabedSampler = m_SeabedSampler.IsValid() ? m_SeabedSampler : m_Sampler;
    device->UpdateCombinedImageSamplerBinding(ds, 3, seabed, seabedSampler);

    const uint32 g8 = (m_Resolution + 7u) / 8u;
    return {{pipe, ds, g8, g8, layers, nextState},
            step, m_PlannedFrame, &plan, substepIndex};
}

bool OceanDynWaves::IsCurrentPlan(const OceanPreparedSimulationDispatch& dispatch) const
{
    return m_Plan && dispatch.Plan == m_Plan &&
        dispatch.Owner.Frame == m_PlannedFrame.Frame &&
        dispatch.Owner.FrameIndex == m_PlannedFrame.FrameIndex &&
        dispatch.SubstepIndex < m_Plan->Steps.size() &&
        dispatch.SubstepIndex == m_NextSubstep &&
        dispatch.Step.WriteIndex < 2u &&
        dispatch.Step.WriteIndex == m_Plan->Steps[dispatch.SubstepIndex].WriteIndex &&
        m_Plan->Textures[dispatch.Step.WriteIndex] == dispatch.Commands.Target &&
        m_State[dispatch.Step.WriteIndex].GetTexture() == dispatch.Commands.Target;
}

void OceanDynWaves::RecordDispatch(CommandList* commands, const OceanPreparedSimulationDispatch& dispatch)
{
    if (!IsCurrentPlan(dispatch) || !RecordOceanPreparedDispatch(commands, dispatch.Commands))
        return;
    m_WriteIndex = dispatch.Step.WriteIndex;
    m_State[m_WriteIndex].CommitLayout(dispatch.Step.Cascade);
    ++m_NextSubstep;
    if (m_NextSubstep == m_Plan->Steps.size())
        FinishSubsteps();
}

} // namespace GameEngine::Ocean
