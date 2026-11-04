#include "Engine/Rendering/RenderSorting.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/SortingLayers.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

TEST(RenderYSorting, GroundPointUsesEntityOriginAndTransformedOffset)
{
    ECS::World world;
    Components::RenderSorting sorting;
    sorting.SortByY = true;
    sorting.SortOriginY = 0.5f;
    Components::WorldTransform transform;
    transform.matrix[5] = -2.0f;
    transform.matrix[13] = -3.0f;
    auto entity = world.Create(sorting, transform);
    world.ProcessCommands();
    const auto data = ResolveRenderSorting(world, entity.GetHandle(), Rendering::SortingLayers::Defaults());
    const float32 position[]{0, 100, -10};
    const float32 forward[]{0, 0, 1};
    const auto key = MakeRenderSortKey(data, 999, position, forward);
    EXPECT_TRUE(data.Authored);
    EXPECT_FLOAT_EQ(key.Depth, -4.0f);
}

TEST(RenderYSorting, ManualLayerAndOrderTakePriorityOverGroundPoint)
{
    RenderSortingData ground;
    ground.SortByY = true;
    ground.SortY = -5;
    const float32 position[]{0, 0, 0};
    const float32 forward[]{0, 0, 1};
    auto front = MakeRenderSortKey(ground, 0, position, forward);
    ground.SortY = 5;
    auto back = MakeRenderSortKey(ground, 0, position, forward);
    EXPECT_TRUE(RenderSortBefore(back, front));
    back.Order = 1;
    EXPECT_TRUE(RenderSortBefore(front, back));
    front.Layer = 1;
    EXPECT_TRUE(RenderSortBefore(back, front));
}

TEST(RenderYSorting, OutermostGroupUsesItsGroundPointAndKeepsMembersContiguous)
{
    ECS::World world;
    Components::RenderSorting sorting;
    sorting.GroupChildren = true;
    sorting.SortByY = true;
    Components::WorldTransform transform;
    transform.matrix[13] = 7;
    auto parent = world.Create(sorting, transform);
    sorting.GroupChildren = false;
    transform.matrix[13] = -10;
    auto child = world.Create(sorting, transform, Components::Parent{parent.GetHandle()});
    world.ProcessCommands();
    const auto data = ResolveRenderSorting(world, child.GetHandle(), Rendering::SortingLayers::Defaults());
    const float32 position[]{0, 0, 0};
    const float32 forward[]{0, 0, 1};
    const auto key = MakeRenderSortKey(data, 100, position, forward);
    EXPECT_EQ(key.Group, parent.GetHandle());
    EXPECT_FLOAT_EQ(key.Depth, 7);
    EXPECT_FLOAT_EQ(key.MemberDepth, -10);
}
