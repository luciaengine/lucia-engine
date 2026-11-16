// The ocean's term in the reflection probe scene digest (IRenderFeature::ContentDigest):
// the surface's presence and its foam and normal textures. A wave or clarity value is not
// content, so an inspector drag does not recapture a "Once" probe.

#include "AssetCore/GUID.h"
#include "Components/Rendering/Ocean.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/RenderServices.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/Systems/OceanExtractionSystem.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Ocean;

namespace
{
constexpr float32 kFrameSeconds = 1.0f / 60.0f;
} // namespace

TEST(OceanContentDigest, TheExtractionPublishesASurfaceThatAppearsAndGoesAway)
{
    ECS::World world;
    Engine::Renderer::RenderServices rs;
    OceanExtractionSystem system(&rs);
    system.Update(world, kFrameSeconds);
    const auto* feature = rs.GetFeature<OceanRenderFeature>();
    ASSERT_NE(feature, nullptr);
    EXPECT_EQ(feature->ContentDigest(world.GetWorldId()), 0u) << "A world without a surface has no ocean content";

    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, Components::OceanSurface{});
    system.Update(world, kFrameSeconds);
    const uint64 withSurface = feature->ContentDigest(world.GetWorldId());
    EXPECT_NE(withSurface, 0u) << "A surface that appears is new content";

    world.GetComponentForWrite<Components::OceanSurface>(entity)->ShallowClarityDistance = 20.0f;
    system.Update(world, kFrameSeconds);
    EXPECT_EQ(feature->ContentDigest(world.GetWorldId()), withSurface) << "A clarity value is not content";

    world.DestroyEntityImmediate(entity);
    system.Update(world, kFrameSeconds);
    EXPECT_EQ(feature->ContentDigest(world.GetWorldId()), 0u) << "A surface that goes away leaves no content";
}

TEST(OceanContentDigest, FollowsTheFoamAndNormalTextures)
{
    const GUID foam = GUID::Generate();
    const GUID normal = GUID::Generate();
    EXPECT_EQ(OceanExtractionSystem::ComputeOceanContentDigest(false, foam, normal), 0u);
    const uint64 base = OceanExtractionSystem::ComputeOceanContentDigest(true, GUID::Null(), GUID::Null());
    EXPECT_NE(base, 0u);
    EXPECT_NE(OceanExtractionSystem::ComputeOceanContentDigest(true, foam, GUID::Null()), base);
    EXPECT_NE(OceanExtractionSystem::ComputeOceanContentDigest(true, GUID::Null(), normal), base);
    EXPECT_EQ(OceanExtractionSystem::ComputeOceanContentDigest(true, foam, normal),
              OceanExtractionSystem::ComputeOceanContentDigest(true, foam, normal));
}
