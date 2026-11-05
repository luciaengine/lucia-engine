#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Ocean/OceanPreparedDispatch.h"
#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"

#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
class CommandList;
} // namespace GameEngine::Rendering

namespace GameEngine::Ocean
{

// Dynamic (interactive) wave simulation. Solves the 2D wave equation on a
// camera-snapped RG16F (height, velocity) cascade and injects per-frame impulses
// (a buoyant body striking the water, a splash), producing spreading, decaying
// ripples layered on top of the FFT spectrum. The dynamic height is
// sampled by the surface vertex modifier (displacement) and fragment shader
// (normal fold).
//
// Ping-pong over two persistent OceanCascadeArrays: read previous, write next —
// the wave equation needs the prior state. One instance lives on
// OceanRenderFeature. The graph orders the prepared dispatch and its readers. A failed init leaves the
// surface on the spectrum waves only (DynamicWavesAvailable stays 0).
//
// The cascade shares the foam cascade's resolution / LOD count / base scale; it
// snaps on the frames it steps, so the surface samples it with its own layout
// (OceanSampledCascade::DynWaves).
class OceanDynWaves
{
public:
    void RebaseOrigin(float32 shiftX, float32 shiftZ)
    {
        m_State[0].RebaseOrigin(shiftX, shiftZ);
        m_State[1].RebaseOrigin(shiftX, shiftZ);
    }
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    uint32 GetResolution() const { return m_Resolution; }
    uint32 GetLodCount() const { return m_LodCount; }

    // Runtime cascade reconfig (OceanRenderer.MinScale / LodDataResolution).
    void ConfigureCascades(::GameEngine::Rendering::IDevice* device, float baseScale, uint32 resolution)
    {
        for (auto& a : m_State)
        {
            if (baseScale > 0.0f) a.SetBaseScale(baseScale);
            if (resolution > 0u) a.Resize(device, resolution);
        }
        m_Resolution = m_State[0].GetResolution();
    }

    // No-frame accessors expose recorded history; frame accessors expose its planned output.
    ::GameEngine::Rendering::TextureHandle GetTexture() const;
    ::GameEngine::Rendering::TextureHandle GetTexture(const Rendering::RenderGraph::RGFrame& frame) const;
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }
    const OceanCascadeLayoutGPU& GetLayout() const;
    const OceanCascadeLayoutGPU& GetLayout(const Rendering::RenderGraph::RGFrame& frame) const;

    // Frame consumers import the planned final output; each step imports its own pair.
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportWriteRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportWriteRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32 writeIndex) const;
    ::GameEngine::Rendering::RenderGraph::RGTexture ImportPrevRG(
        ::GameEngine::Rendering::RenderGraph::RGFrame& frame, uint32 writeIndex) const;

    /// Render-thread declaration; the frame owns the plan until it resets or is destroyed.
    /// Call ForgetFrame before destroying the stream; planning does not commit history.
    const OceanCascadeFramePlan& PlanSubsteps(Rendering::RenderGraph::RGFrame& frame,
                                             float cameraX, float cameraZ);

    /// Render-thread retirement releases borrowed plan pointers and preserves recorded history.
    void ForgetFrame(const Rendering::RenderGraph::RGFrame& frame)
    {
        if (m_PlannedFrame.Frame == &frame)
        {
            m_PlannedFrame = {};
            m_Plan = nullptr;
            m_NextSubstep = 0;
        }
    }

    /// Retain unconsumed work until quiescence expires it, then append this frame's inputs.
    /// The bounded impulse list retains the oldest pending inputs first.
    void SetFrameInputs(const OceanWaveImpulseGPU* impulses, uint32 count, float waveSpeed,
                        float damping, float deltaTime);
    void ConfigureSimulation(float32 frequency, uint32 maximumSubsteps,
                             float32 damping, float32 courantNumber, float32 gravity,
                             float32 shallowAttenuation, float32 horizontalDisplacement,
                             float32 displacementClamp, uint32 minimumCascade,
                             uint32 maximumCascade);
    void ClearSimulationConfiguration();
    void SetShallowWaterInputs(::GameEngine::Rendering::TextureHandle seabedDepth,
                               ::GameEngine::Rendering::SamplerHandle seabedSampler,
                               bool available);
    uint32 GetPendingSubstepCount() const { return m_PendingSubsteps; }
    void FinishSubsteps();
    void InvalidateHistory()
    {
        m_ResetHistory = true;
        m_TimeAccumulator = 0.0f;
    }

    // True once the sim has advanced impulse-free for enough SIMULATED time that
    // the damped field is imperceptibly flat. Wall/render frames do not count:
    // TimeScale=0 and fixed-time capture must preserve a frozen disturbance.
    bool IsQuiescent() const { return m_SimulatedSecondsSinceImpulse >= kQuiescentSeconds; }

    // Upload one immutable step's parameters before preparing its descriptor bindings.
    void FillParams(OceanDynWavesParamsGPU& out, const OceanCascadeStep& step,
                    uint32 substepIndex) const;

    /// Declare this ID with RGPassBuilder::UsePipeline before preparation.
    ::GameEngine::Rendering::ComputePipelineId GetPipelineId() const { return m_Pipe; }

    /// Render-thread preparation borrows this frame's plan and upload; it does not consume steps.
    OceanPreparedSimulationDispatch PrepareDispatch(Rendering::IDevice* device,
        Rendering::BufferHandle paramsBuffer, uint64 paramsOffset,
        const OceanCascadeFramePlan& plan, uint32 substepIndex) const;
    /// Record on the render thread in substep order; stale or consumed plans do not dispatch.
    void RecordDispatch(Rendering::CommandList* commands,
                        const OceanPreparedSimulationDispatch& dispatch);

private:
    bool IsCurrentPlan(const OceanPreparedSimulationDispatch& dispatch) const;

    // Match the foam cascade so the layouts coincide (see OceanFoamSim).
    static constexpr uint32 kDynResolution = 256;
    static constexpr uint32 kDynLodCount = 4;
    static constexpr float kDynBaseScale = 64.0f;
    // At the default 0.2/s velocity damping this leaves less than 0.25% of the
    // original velocity energy before the stale camera-snapped cascade is hidden.
    static constexpr float32 kQuiescentSeconds = 30.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    uint32 m_Resolution = kDynResolution;
    uint32 m_LodCount = kDynLodCount;

    // Only recorded steps change the index and persistent cascade layouts.
    OceanCascadeArray m_State[2];
    uint32 m_WriteIndex = 0;
    Rendering::RenderGraph::RGFrameStamp m_PlannedFrame;
    const OceanCascadeFramePlan* m_Plan = nullptr;
    uint32 m_NextSubstep = 0;

    // Simulated seconds since the last impulse (advanced by SetFrameInputs' dt,
    // reset by a non-empty list). Starts quiescent: a scene with no interaction
    // never dispatches.
    float32 m_SimulatedSecondsSinceImpulse = kQuiescentSeconds;
    bool m_UseFixedStep = false;
    float32 m_SimulationFrequency = 60.0f;
    uint32 m_MaximumSubsteps = 4u;
    float32 m_TimeAccumulator = 0.0f;
    float32 m_SubstepDelta = 0.0f;
    uint32 m_PendingSubsteps = 0u;
    uint32 m_PendingImpulseCount = 0u;
    bool m_ResetHistory = false;

    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear/clamp for sim + surface read

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    OceanDynWavesParamsGPU m_Params;
    ::GameEngine::Rendering::TextureHandle m_SeabedDepth;
    ::GameEngine::Rendering::SamplerHandle m_SeabedSampler;
};

} // namespace GameEngine::Ocean
