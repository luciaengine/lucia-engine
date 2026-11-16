#pragma once

#include "Mathematics/Quaternion.h"
#include "Mathematics/WorldPosition.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{

/// One held camera pose of a benchmark run.
struct PlayerBenchmarkPose
{
    std::string Name;
    /// World meters.
    Mathematics::WorldPosition Position;
    /// Degrees about +Y. 0 looks along +Z, 90 looks along +X.
    double YawDeg = 0.0;
    /// Degrees. A positive pitch looks up.
    double PitchDeg = 0.0;

    /// The camera rotation of this pose: the yaw, then the pitch about the yawed +X.
    Mathematics::Quaternion Rotation() const;
};

/// What a --benchmark run measures: for each pose, warm-up frames, then frames timed with GPU
/// profiling off, then frames with per-pass GPU timings.
struct PlayerBenchmarkPlan
{
    uint32_t WarmupFrames = 300;
    uint32_t TimingFrames = 600;
    uint32_t ProfiledFrames = 120;
    std::vector<PlayerBenchmarkPose> Poses;
};

/// Reads a plan from its JSON document. Nullopt, with the reason and its fix in `error`, when a
/// field has the wrong type or range or the plan has no pose.
std::optional<PlayerBenchmarkPlan> ParsePlayerBenchmarkPlan(const nlohmann::json& document, std::string& error);

/// Reads the plan file at `path`. Nullopt, with the reason and its fix logged, when the file cannot
/// be read, is not JSON or is refused by ParsePlayerBenchmarkPlan.
std::optional<PlayerBenchmarkPlan> LoadPlayerBenchmarkPlan(const std::filesystem::path& path);

} // namespace GameEngine
