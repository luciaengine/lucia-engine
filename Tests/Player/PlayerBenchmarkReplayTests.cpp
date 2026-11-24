#include "Benchmark/PlayerBenchmark.h"

#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TestTempDir.h"

#include <array>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{

const auto kFlyoverPlan = R"({
    "warmupFrames": 2, "timingFrames": 3, "profiledFrames": 1,
    "poses": [{"position": [0, 550, 0]}],
    "timingPath": [{"position": [0, 500, 1], "yawDeg": 0, "pitchDeg": 0},
                   {"position": [0, 150, 2], "yawDeg": 10, "pitchDeg": -5},
                   {"position": [0, 18, 3], "yawDeg": 20, "pitchDeg": -10}]
})";

TEST(PlayerBenchmarkReplay, AppliesEachTimedPoseThenProfilesAtTheLastPose)
{
    std::string error;
    const auto plan = ParsePlayerBenchmarkPlan(nlohmann::json::parse(kFlyoverPlan), error);
    ASSERT_TRUE(plan) << error;
    const TestUtils::ScopedTempDir scratch(TestUtils::MakeUniqueTempDirectory("ge_benchmark_replay"));
    const auto output = scratch.Path() / "replay.json";
    ECS::World world(nullptr);
    const auto camera = world.Create().GetHandle();
    world.ProcessCommands();
    world.AddComponentImmediate<Components::Camera>(camera, {});
    world.AddComponentImmediate<Components::Transform>(camera, {});
    world.AddComponentImmediate<Components::WorldTransform>(camera, {});
    PlayerBenchmark benchmark(*plan, output);
    const std::array heights{550.0, 550.0, 500.0, 150.0, 18.0, 18.0, 18.0, 18.0, 18.0, 18.0};
    for (double height : heights)
    {
        ASSERT_FALSE(benchmark.Advance(world, nullptr, nullptr, true));
        EXPECT_DOUBLE_EQ(world.GetComponent<Components::Transform>(camera)->GetPosition().Y, height);
        EXPECT_DOUBLE_EQ(world.GetComponent<Components::WorldTransform>(camera)->GetPosition().Y, height);
    }
    ASSERT_TRUE(benchmark.Advance(world, nullptr, nullptr, true));
    ASSERT_TRUE(benchmark.Succeeded());
    std::ifstream savedResult(output);
    const auto result = nlohmann::json::parse(savedResult);
    EXPECT_EQ(result.at("timingPath"), nlohmann::json::parse(kFlyoverPlan).at("timingPath"));
    EXPECT_EQ(result.at("poses").at(0).at("frameMs").size(), 3u);
    EXPECT_EQ(result.at("profiledPose"), result.at("timingPath").back());
}

TEST(PlayerBenchmarkReplay, RefusesAPathWithoutAnActiveCamera)
{
    std::string error;
    const auto plan = ParsePlayerBenchmarkPlan(nlohmann::json::parse(kFlyoverPlan), error);
    ASSERT_TRUE(plan) << error;
    const TestUtils::ScopedTempDir scratch(TestUtils::MakeUniqueTempDirectory("ge_benchmark_no_camera"));
    const auto output = scratch.Path() / "replay.json";
    ECS::World world(nullptr);
    PlayerBenchmark benchmark(*plan, output);
    EXPECT_TRUE(benchmark.Advance(world, nullptr, nullptr, true));
    EXPECT_FALSE(benchmark.Succeeded());
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(PlayerBenchmarkReplay, RefusesToMeasureAFrameThatWasNotSubmitted)
{
    std::string error;
    const auto plan = ParsePlayerBenchmarkPlan(nlohmann::json::parse(kFlyoverPlan), error);
    ASSERT_TRUE(plan) << error;
    const TestUtils::ScopedTempDir scratch(TestUtils::MakeUniqueTempDirectory("ge_benchmark_no_submit"));
    const auto output = scratch.Path() / "replay.json";
    ECS::World world(nullptr);
    const auto camera = world.Create().GetHandle();
    world.ProcessCommands();
    world.AddComponentImmediate<Components::Camera>(camera, {});
    world.AddComponentImmediate<Components::Transform>(camera, {});
    PlayerBenchmark benchmark(*plan, output);

    EXPECT_FALSE(benchmark.Advance(world, nullptr, nullptr, false));
    EXPECT_TRUE(benchmark.Advance(world, nullptr, nullptr, false));
    EXPECT_FALSE(benchmark.Succeeded());
    EXPECT_FALSE(std::filesystem::exists(output));
}

} // namespace
} // namespace GameEngine
