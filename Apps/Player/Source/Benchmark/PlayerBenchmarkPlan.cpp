#include "Benchmark/PlayerBenchmarkPlan.h"

#include "Logger/Logger.h"
#include "Mathematics/Angle.h"

#include <cmath>
#include <fstream>
#include <limits>

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

// A frame count field, or the default when absent. False when present and not a count in [minimum, 2^32).
bool ReadFrameCount(const nlohmann::json& document, const char* key, uint32_t minimum, uint32_t& out,
                    std::string& error)
{
    const auto it = document.find(key);
    if (it == document.end())
        return true;
    if (!it->is_number_unsigned() || it->get<uint64_t>() < minimum ||
        it->get<uint64_t>() > std::numeric_limits<uint32_t>::max())
    {
        error = std::string("'") + key + "' must be a whole frame count of at least " + std::to_string(minimum);
        return false;
    }
    out = it->get<uint32_t>();
    return true;
}

bool ReadFiniteNumber(const nlohmann::json& pose, const char* key, double& out, std::string& error)
{
    const auto it = pose.find(key);
    if (it == pose.end())
        return true;
    if (!it->is_number() || !std::isfinite(it->get<double>()))
    {
        error = std::string("pose '") + key + "' must be a finite number of degrees";
        return false;
    }
    out = it->get<double>();
    return true;
}

std::optional<PlayerBenchmarkPose> ParsePose(const nlohmann::json& pose, size_t index, std::string& error)
{
    PlayerBenchmarkPose result;
    result.Name = "pose" + std::to_string(index);
    if (!pose.is_object())
    {
        error = "each pose must be an object with \"position\": [x, y, z], \"yawDeg\" and \"pitchDeg\"";
        return std::nullopt;
    }
    if (const auto name = pose.find("name"); name != pose.end())
    {
        if (!name->is_string() || name->get<std::string>().empty())
        {
            error = "pose 'name' must be a non-empty string";
            return std::nullopt;
        }
        result.Name = name->get<std::string>();
    }
    const auto position = pose.find("position");
    if (position == pose.end() || !position->is_array() || position->size() != 3)
    {
        error = "pose '" + result.Name + "' needs \"position\": [x, y, z] in meters";
        return std::nullopt;
    }
    for (const auto& value : *position)
    {
        if (!value.is_number() || !std::isfinite(value.get<double>()))
        {
            error = "pose '" + result.Name + "' position must hold 3 finite numbers";
            return std::nullopt;
        }
    }
    result.Position = Mathematics::WorldPosition{(*position)[0].get<double>(), (*position)[1].get<double>(),
                                                 (*position)[2].get<double>()};
    if (!ReadFiniteNumber(pose, "yawDeg", result.YawDeg, error) ||
        !ReadFiniteNumber(pose, "pitchDeg", result.PitchDeg, error))
        return std::nullopt;
    return result;
}

} // namespace

Mathematics::Quaternion PlayerBenchmarkPose::Rotation() const
{
    // About +X a positive angle turns +Z toward -Y, so a pitch that looks up is a negative angle.
    const Mathematics::Quaternion yaw =
        Mathematics::Quaternion::FromAxisAngle(Mathematics::Vector3(0.0f, 1.0f, 0.0f), Mathematics::ToRadians(static_cast<float>(YawDeg)));
    const Mathematics::Quaternion pitch = Mathematics::Quaternion::FromAxisAngle(
        Mathematics::Vector3(1.0f, 0.0f, 0.0f), -Mathematics::ToRadians(static_cast<float>(PitchDeg)));
    return yaw * pitch;
}

std::optional<PlayerBenchmarkPlan> ParsePlayerBenchmarkPlan(const nlohmann::json& document, std::string& error)
{
    if (!document.is_object())
    {
        error = "the plan must be a JSON object with a \"poses\" array";
        return std::nullopt;
    }
    PlayerBenchmarkPlan plan;
    if (!ReadFrameCount(document, "warmupFrames", 0, plan.WarmupFrames, error) ||
        !ReadFrameCount(document, "timingFrames", 2, plan.TimingFrames, error) ||
        !ReadFrameCount(document, "profiledFrames", 1, plan.ProfiledFrames, error))
        return std::nullopt;

    if (const auto monitors = document.find("activeMonitors"); monitors != document.end())
    {
        if (!monitors->is_array())
        {
            error = "'activeMonitors' must be an array of monitor names";
            return std::nullopt;
        }
        for (const auto& monitor : *monitors)
        {
            if (!monitor.is_string() || monitor.get_ref<const std::string&>().empty())
            {
                error = "each 'activeMonitors' entry must be a non-empty monitor name";
                return std::nullopt;
            }
            plan.ActiveMonitors.push_back(monitor.get<std::string>());
        }
    }

    const auto poses = document.find("poses");
    if (poses == document.end() || !poses->is_array() || poses->empty())
    {
        error = "the plan needs a non-empty \"poses\" array";
        return std::nullopt;
    }
    for (size_t i = 0; i < poses->size(); ++i)
    {
        std::optional<PlayerBenchmarkPose> pose = ParsePose((*poses)[i], i, error);
        if (!pose)
            return std::nullopt;
        plan.Poses.push_back(std::move(*pose));
    }
    if (const auto path = document.find("timingPath"); path != document.end())
    {
        if (!path->is_array() || path->size() != plan.TimingFrames || plan.Poses.size() != 1)
        {
            error = "'timingPath' needs one entry per timing frame and exactly one warm-up pose";
            return std::nullopt;
        }
        plan.TimingPath.reserve(path->size());
        for (size_t frame = 0; frame < path->size(); ++frame)
        {
            auto pose = ParsePose((*path)[frame], frame, error);
            if (!pose)
            {
                error = "'timingPath' frame " + std::to_string(frame) + ": " + error;
                return std::nullopt;
            }
            plan.TimingPath.push_back(std::move(*pose));
        }
    }
    return plan;
}

std::optional<PlayerBenchmarkPlan> LoadPlayerBenchmarkPlan(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        Logger::Log::Error("Player: cannot read the benchmark plan '{}'. Check the --benchmark path.", path.string());
        return std::nullopt;
    }
    const nlohmann::json document = nlohmann::json::parse(in, nullptr, false);
    if (document.is_discarded())
    {
        Logger::Log::Error("Player: the benchmark plan '{}' is not valid JSON.", path.string());
        return std::nullopt;
    }
    std::string error;
    std::optional<PlayerBenchmarkPlan> plan = ParsePlayerBenchmarkPlan(document, error);
    if (!plan)
        Logger::Log::Error("Player: the benchmark plan '{}' is refused: {}.", path.string(), error);
    return plan;
}

} // namespace GameEngine
