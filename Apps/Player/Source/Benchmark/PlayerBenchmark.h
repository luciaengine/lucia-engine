#pragma once

#include "Benchmark/PlayerBenchmarkPlan.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace Rendering
{
class IDevice;
}
namespace Rendering::RenderGraph
{
class RGFrame;
}

/// --benchmark records raw frame data for held poses or an ordered camera path. It warms up first,
/// times frames with profiling off, then profiles at a held pose. The result is one JSON file.
///
/// Frame time is the wall time between two Advance calls. CPU time is the frame time less the
/// time the frame waited for the GPU and the swapchain (fence wait, acquire and present).
class PlayerBenchmark
{
  public:
    PlayerBenchmark(PlayerBenchmarkPlan plan, std::filesystem::path outputPath);

    /// Poses the active camera before rendering and records the previous frame. Route mode
    /// requires a submitted previous frame after the first call. True when the caller should exit.
    bool Advance(ECS::World& world, Rendering::RenderGraph::RGFrame* frame, const Rendering::IDevice* device,
                 bool previousFrameSubmitted);
    bool UsesTimingPath() const { return !m_Plan.TimingPath.empty(); }

    /// False when the result could not be written; the reason is logged.
    bool Succeeded() const { return m_Succeeded; }

  private:
    enum class Phase
    {
        Warmup,
        Timing,
        ProfileLead,
        Profiled,
        Done,
    };

    struct PassSample
    {
        std::string Name;
        double GpuMs = 0.0;
        bool Counted = false;
    };

    struct ProfiledFrame
    {
        double DistinctGpuMs = 0.0;
        std::vector<PassSample> Passes;
    };

    struct PoseRecord
    {
        std::vector<double> FrameMs;
        std::vector<double> CpuMs;
        std::vector<double> WaitMs;
        std::vector<ProfiledFrame> Profiled;
        std::vector<float> MonitorPeaks;
    };

    bool ApplyPose(ECS::World& world) const;
    void RecordTimingFrame(double frameMs, const Rendering::IDevice* device);
    void RecordProfiledFrame(const Rendering::RenderGraph::RGFrame& frame);
    void EnterPhase(Phase phase, Rendering::RenderGraph::RGFrame* frame);
    bool FailRoute(const char* reason, Rendering::RenderGraph::RGFrame* frame);
    void WriteResult();

    PlayerBenchmarkPlan m_Plan;
    std::filesystem::path m_OutputPath;
    std::vector<PoseRecord> m_Records;
    size_t m_PoseIndex = 0;
    Phase m_Phase = Phase::Warmup;
    uint32_t m_PhaseFrames = 0;
    std::optional<std::chrono::steady_clock::time_point> m_LastAdvance;
    bool m_Succeeded = false;
};

} // namespace GameEngine
