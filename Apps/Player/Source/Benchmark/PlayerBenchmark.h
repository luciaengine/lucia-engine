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

/// --benchmark: holds the active camera at each pose of a plan and records raw frame data. For each
/// pose it renders the warm-up frames, then the timing frames with GPU profiling off, then the
/// profiled frames with per-pass GPU timings. After the last pose it writes one JSON file.
///
/// Frame time is the wall time between two Advance calls. CPU time is the frame time less the
/// time the frame waited for the GPU and the swapchain (fence wait, acquire and present).
class PlayerBenchmark
{
  public:
    PlayerBenchmark(PlayerBenchmarkPlan plan, std::filesystem::path outputPath);

    /// Call once per frame after the startup scene is ready, before the frame renders. Poses the
    /// active camera and records the frame that ended. True when the run is complete and its
    /// result is written: the caller exits.
    bool Advance(ECS::World& world, Rendering::RenderGraph::RGFrame* frame, const Rendering::IDevice* device);

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

    void ApplyPose(ECS::World& world) const;
    void RecordTimingFrame(double frameMs, const Rendering::IDevice* device);
    void RecordProfiledFrame(const Rendering::RenderGraph::RGFrame& frame);
    void EnterPhase(Phase phase, Rendering::RenderGraph::RGFrame* frame);
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
