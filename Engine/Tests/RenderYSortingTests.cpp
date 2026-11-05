#include "Engine/Rendering/RenderSorting.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Rendering/WorldSectorCoord.h"
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

TEST(RenderYSorting, GroundPointCarriesTheSectorOffsetForTileRows)
{
    ECS::World world;
    Components::RenderSorting sorting;
    sorting.SortByY = true;
    auto entity = world.Create(sorting, Components::WorldTransform{}, Components::WorldSectorCoord{0, 1, 0});
    world.ProcessCommands();
    const auto data = ResolveRenderSorting(world, entity.GetHandle(), Rendering::SortingLayers::Defaults());
    EXPECT_FLOAT_EQ(data.SectorOffsetY, Components::kWorldSectorSize);
    EXPECT_FLOAT_EQ(data.SortY, Components::kWorldSectorSize);
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

TEST(RenderYSorting, GroupPresenceTracksAuthoredChangesIncludingDisabledAncestors)
{
    ECS::World world;
    EXPECT_FALSE(HasRenderSortingGroups(world));
    Components::RenderSorting sorting;
    sorting.SortByY = true;
    auto parent = world.Create(sorting, Components::WorldTransform{});
    world.ProcessCommands();
    EXPECT_FALSE(HasRenderSortingGroups(world));
    sorting.GroupChildren = true;
    parent.Set(sorting);
    world.ProcessCommands();
    EXPECT_TRUE(HasRenderSortingGroups(world));
    parent.SetEnabled(false);
    world.ProcessCommands();
    EXPECT_TRUE(HasRenderSortingGroups(world));
    parent.Remove<Components::RenderSorting>();
    world.ProcessCommands();
    EXPECT_FALSE(HasRenderSortingGroups(world));
}

TEST(RenderYSorting, NoGroupGuardPreservesOwnGroundSortingAndGroupDiscovery)
{
    ECS::World world;
    auto parent = world.Create(Components::WorldTransform{});
    Components::RenderSorting sorting;
    sorting.Layer = 4;
    sorting.OrderInLayer = -3;
    sorting.SortByY = true;
    Components::WorldTransform transform;
    transform.matrix[13] = -7;
    auto child = world.Create(sorting, transform, Components::Parent{parent.GetHandle()});
    world.ProcessCommands();
    const auto layers = Rendering::SortingLayers::Defaults();
    const auto guarded = ResolveRenderSorting(world, child.GetHandle(), layers, HasRenderSortingGroups(world));
    const auto original = ResolveRenderSorting(world, child.GetHandle(), layers);
    EXPECT_EQ(guarded.Layer, original.Layer);
    EXPECT_EQ(guarded.Order, original.Order);
    EXPECT_EQ(guarded.MemberOrder, original.MemberOrder);
    EXPECT_EQ(guarded.SortByY, original.SortByY);
    EXPECT_FLOAT_EQ(guarded.SortOriginY, original.SortOriginY);
    EXPECT_EQ(guarded.Grouped, original.Grouped);
    EXPECT_EQ(guarded.GroupSortByY, original.GroupSortByY);
    for (int axis = 0; axis < 3; ++axis)
        EXPECT_FLOAT_EQ(guarded.GroupPosition[axis], original.GroupPosition[axis]);
    EXPECT_EQ(guarded.Authored, original.Authored);
    EXPECT_EQ(guarded.Group, original.Group);
    EXPECT_FLOAT_EQ(guarded.SortY, original.SortY);
    sorting.GroupChildren = true;
    parent.Set(sorting);
    world.ProcessCommands();
    EXPECT_TRUE(HasRenderSortingGroups(world));
    EXPECT_EQ(ResolveRenderSorting(world, child.GetHandle(), layers, HasRenderSortingGroups(world)).Group,
              parent.GetHandle());
}

TEST(RenderYSorting, BatchGuardIsLazyAndBoundsUnrelatedSortingScan)
{
    ECS::World world;
    auto parent = world.Create(Components::WorldTransform{});
    auto child = world.Create(Components::WorldTransform{}, Components::Parent{parent.GetHandle()});
    world.ProcessCommands();
    std::optional<bool> groups;
    EXPECT_FALSE(groups.has_value());
    const auto layers = Rendering::SortingLayers::Defaults();
    ResolveRenderSortingForBatch(world, child.GetHandle(), layers, groups, 1);
    ASSERT_TRUE(groups.has_value());
    EXPECT_FALSE(*groups);
    for (int index = 0; index < 3; ++index)
        world.Create(Components::RenderSorting{});
    world.ProcessCommands();
    groups.reset();
    ResolveRenderSortingForBatch(world, child.GetHandle(), layers, groups, 1);
    ASSERT_TRUE(groups.has_value());
    EXPECT_TRUE(*groups);
    Components::RenderSorting sorting;
    sorting.GroupChildren = true;
    parent.Set(sorting);
    parent.SetEnabled(false);
    world.ProcessCommands();
    groups.reset();
    const auto guarded = ResolveRenderSortingForBatch(world, child.GetHandle(), layers, groups, 10);
    const auto original = ResolveRenderSorting(world, child.GetHandle(), layers);
    EXPECT_EQ(guarded.Group, original.Group);
    EXPECT_EQ(guarded.Group, parent.GetHandle());
}
