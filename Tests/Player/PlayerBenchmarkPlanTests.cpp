// The --benchmark plan: the frame counts, the poses, and the camera rotation of a pose. A plan
// that a typo makes wrong must be refused, because a benchmark that runs a different plan gives a
// number that compares with nothing.

#include "Benchmark/PlayerBenchmarkPlan.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

using GameEngine::ParsePlayerBenchmarkPlan;
using GameEngine::PlayerBenchmarkPlan;
using GameEngine::PlayerBenchmarkPose;

namespace
{
std::optional<PlayerBenchmarkPlan> Parse(const char* text)
{
    std::string error;
    return ParsePlayerBenchmarkPlan(nlohmann::json::parse(text), error);
}

void ExpectLooksAlong(const PlayerBenchmarkPose& pose, float x, float y, float z)
{
    const GameEngine::Mathematics::Vector3 forward = pose.Rotation().Rotate(GameEngine::Mathematics::Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(forward.x, x, 1e-5f);
    EXPECT_NEAR(forward.y, y, 1e-5f);
    EXPECT_NEAR(forward.z, z, 1e-5f);
}
} // namespace

TEST(PlayerBenchmarkPlan, ReadsTheFrameCountsAndEveryPose)
{
    const std::optional<PlayerBenchmarkPlan> plan = Parse(R"({
        "warmupFrames": 10, "timingFrames": 20, "profiledFrames": 5,
        "poses": [{"name": "street", "position": [1, 2, 3], "yawDeg": 90, "pitchDeg": -10},
                  {"position": [0, 50, 0]}]})");
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->WarmupFrames, 10u);
    EXPECT_EQ(plan->TimingFrames, 20u);
    EXPECT_EQ(plan->ProfiledFrames, 5u);
    ASSERT_EQ(plan->Poses.size(), 2u);
    EXPECT_EQ(plan->Poses[0].Name, "street");
    EXPECT_DOUBLE_EQ(plan->Poses[0].Position.Z, 3.0);
    EXPECT_DOUBLE_EQ(plan->Poses[0].YawDeg, 90.0);
    EXPECT_DOUBLE_EQ(plan->Poses[0].PitchDeg, -10.0);
    EXPECT_EQ(plan->Poses[1].Name, "pose1");
    EXPECT_DOUBLE_EQ(plan->Poses[1].Position.Y, 50.0);
}

TEST(PlayerBenchmarkPlan, RefusesAPlanThatCannotMeasure)
{
    EXPECT_FALSE(Parse(R"({"poses": []})"));
    EXPECT_FALSE(Parse(R"({"timingFrames": 1, "poses": [{"position": [0, 0, 0]}]})"));
    EXPECT_FALSE(Parse(R"({"profiledFrames": 0, "poses": [{"position": [0, 0, 0]}]})"));
    EXPECT_FALSE(Parse(R"({"warmupFrames": -1, "poses": [{"position": [0, 0, 0]}]})"));
    EXPECT_FALSE(Parse(R"({"timingFrames": "600", "poses": [{"position": [0, 0, 0]}]})"));
}

TEST(PlayerBenchmarkPlan, ReadsMonitorNamesAndRefusesInvalidEntries)
{
    const auto plan = Parse(R"({"poses": [{"position": [0, 0, 0]}],
                                "activeMonitors": ["ClusterLOD.SplitInstances"]})");
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->ActiveMonitors.size(), 1u);
    EXPECT_EQ(plan->ActiveMonitors.front(), "ClusterLOD.SplitInstances");
    EXPECT_FALSE(Parse(R"({"poses": [{"position": [0, 0, 0]}], "activeMonitors": "ClusterLOD.SplitInstances"})"));
    EXPECT_FALSE(Parse(R"({"poses": [{"position": [0, 0, 0]}], "activeMonitors": [""]})"));
    EXPECT_FALSE(Parse(R"({"poses": [{"position": [0, 0, 0]}], "activeMonitors": [1]})"));
}

TEST(PlayerBenchmarkPlan, RefusesAPoseWithoutAFullPosition)
{
    EXPECT_FALSE(Parse(R"({"poses": [{"name": "a"}]})"));
    EXPECT_FALSE(Parse(R"({"poses": [{"position": [0, 0]}]})"));
    EXPECT_FALSE(Parse(R"({"poses": [{"position": [0, "1", 0]}]})"));
    EXPECT_FALSE(Parse(R"({"poses": [{"position": [0, 0, 0], "yawDeg": "north"}]})"));
    EXPECT_FALSE(Parse(R"({"poses": [{"position": [0, 0, 0], "name": ""}]})"));
}

TEST(PlayerBenchmarkPlan, YawTurnsFromPlusZTowardPlusXAndPositivePitchLooksUp)
{
    PlayerBenchmarkPose pose;
    ExpectLooksAlong(pose, 0.0f, 0.0f, 1.0f);
    pose.YawDeg = 90.0;
    ExpectLooksAlong(pose, 1.0f, 0.0f, 0.0f);
    pose.YawDeg = 0.0;
    pose.PitchDeg = 90.0;
    ExpectLooksAlong(pose, 0.0f, 1.0f, 0.0f);
    pose.YawDeg = 90.0;
    pose.PitchDeg = -45.0;
    ExpectLooksAlong(pose, 0.70710678f, -0.70710678f, 0.0f);
}
