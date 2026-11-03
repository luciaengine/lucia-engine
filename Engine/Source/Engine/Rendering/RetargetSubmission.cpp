#include "Engine/Rendering/RetargetSubmission.h"

#include "Platform/Environment.h"

#include <atomic>
#include <mutex>

namespace GameEngine { namespace Engine { namespace Renderer {

namespace
{

// 0 = unread, 1 = CPU only, 2 = GPU allowed.
std::atomic<uint8_t> g_CpuOnlyState{0};

// LOD distance thresholds (meters). Tuned for typical 100-character crowd
// shots; can move into a config asset later.
constexpr float kLODNoOpsDistanceMeters    = 15.0f;
constexpr float kLODFKOnlyDistanceMeters   = 35.0f;
constexpr float kLODPoseHoldDistanceMeters = 80.0f;

std::mutex            g_StatsMutex;
RetargetFrameStats    g_LastFrameStats{};

bool ReadEnvFlag()
{
    return Platform::EnvironmentSwitchEnabled("GE_RETARGET_CPU", false) ||
           Platform::EnvironmentSwitchEnabled("r.RetargetCPU", false);
}

} // namespace

bool IsRetargetCPUOnly()
{
    uint8_t state = g_CpuOnlyState.load(std::memory_order_relaxed);
    if (state == 0)
    {
        const bool cpuOnly = ReadEnvFlag();
        state = cpuOnly ? 1 : 2;
        g_CpuOnlyState.store(state, std::memory_order_relaxed);
    }
    return state == 1;
}

void ResetRetargetCPUToggle()
{
    g_CpuOnlyState.store(0, std::memory_order_relaxed);
}

::GameEngine::Components::HumanoidRetargetLOD SelectRetargetLOD(const LODInputs& inputs)
{
    using ::GameEngine::Components::HumanoidRetargetLOD;

    // Off-screen + far: hold the previous frame's pose entirely.
    if (!inputs.OnScreen && inputs.DistanceMeters >= kLODFKOnlyDistanceMeters)
        return HumanoidRetargetLOD::PoseHold;

    HumanoidRetargetLOD level = HumanoidRetargetLOD::Full;
    if (inputs.DistanceMeters >= kLODPoseHoldDistanceMeters)
        level = HumanoidRetargetLOD::PoseHold;
    else if (inputs.DistanceMeters >= kLODFKOnlyDistanceMeters)
        level = HumanoidRetargetLOD::FKOnly;
    else if (inputs.DistanceMeters >= kLODNoOpsDistanceMeters || !inputs.OnScreen)
        level = HumanoidRetargetLOD::NoOps;

    // Component-level IK off forces at least FKOnly (skip Stage 5).
    if (!inputs.IKEnabledByComponent && level < HumanoidRetargetLOD::FKOnly)
        level = HumanoidRetargetLOD::FKOnly;

    return level;
}

RetargetFrameStats GetLastFrameStats()
{
    std::lock_guard<std::mutex> lk(g_StatsMutex);
    return g_LastFrameStats;
}

void ResetFrameStats()
{
    std::lock_guard<std::mutex> lk(g_StatsMutex);
    g_LastFrameStats = RetargetFrameStats{};
}

void WriteFrameStats(const RetargetFrameStats& stats)
{
    std::lock_guard<std::mutex> lk(g_StatsMutex);
    g_LastFrameStats = stats;
}

}}} // namespace GameEngine::Engine::Renderer
