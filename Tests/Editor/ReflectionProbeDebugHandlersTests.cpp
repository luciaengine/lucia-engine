// get_reflection_probe_state's reply: one entry per retained probe of the world, with the
// entity id, the update mode and the digests a recapture check compares.

#include "DebugServer/ReflectionProbeDebugHandlers.h"

#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{
constexpr float32 kFrameSeconds = 1.0f / 60.0f;
// A box probe 4 m on a side.
constexpr float32 kProbeScale = 4.0f;

ECS::EntityHandle AddOnceProbe(ECS::World& world, float x)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::ReflectionProbe probe{};
    probe.UpdateMode = Components::ReflectionProbeUpdateMode::Once;
    world.AddComponentImmediate(entity, probe);
    Components::WorldTransform transform{};
    transform.Basis[0].x = kProbeScale;
    transform.Basis[1].y = kProbeScale;
    transform.Basis[2].z = kProbeScale;
    transform.Position = Mathematics::WorldPosition{x, 0.0, 0.0};
    world.AddComponentImmediate(entity, transform);
    return entity;
}
} // namespace

TEST(ReflectionProbeDebugHandlers, TheStateListsEveryRetainedProbeWithItsDigests)
{
    ECS::World world;
    const ECS::EntityHandle west = AddOnceProbe(world, 0.0f);
    const ECS::EntityHandle east = AddOnceProbe(world, 20.0f);
    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    system.Update(world, kFrameSeconds);

    const nlohmann::json state = DescribeReflectionProbes(rs.GetFeature<ImageBasedLightingFeature>(), world.GetWorldId());
    EXPECT_EQ(state["worldId"].get<uint64>(), world.GetWorldId());
    EXPECT_EQ(state["capacity"].get<uint32_t>(), ImageBasedLightingFeature::kMaxLocalProbes);
    const auto& probes = state["probes"];
    ASSERT_EQ(probes.size(), 2u);
    std::vector<uint64> entities;
    for (const auto& probe : probes)
    {
        entities.push_back(probe["entityId"].get<uint64>());
        EXPECT_EQ(probe["updateMode"].get<uint32_t>(), static_cast<uint32_t>(SceneReflectionProbeUpdateMode::Once));
        EXPECT_NE(probe["sceneEditDigest"].get<std::string>(), "0");
        EXPECT_NE(probe["inputDigest"].get<std::string>(), "0");
        EXPECT_FALSE(probe["ready"].get<bool>()) << "No device: the capture has not baked";
    }
    EXPECT_NE(std::find(entities.begin(), entities.end(), west.id & 0xffffffffu), entities.end());
    EXPECT_NE(std::find(entities.begin(), entities.end(), east.id & 0xffffffffu), entities.end());
}

TEST(ReflectionProbeDebugHandlers, AWorldWithoutTheFeatureListsNoProbe)
{
    const nlohmann::json state = DescribeReflectionProbes(nullptr, 7u);
    EXPECT_EQ(state["worldId"].get<uint64>(), 7u);
    EXPECT_TRUE(state["probes"].empty());
}
