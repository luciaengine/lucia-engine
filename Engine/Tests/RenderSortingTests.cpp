#include "Engine/Rendering/AuthoredTransparentRuns.h"
#include "Engine/Rendering/SortedTransparentComposition.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Rendering/RenderSorting.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"

#include <gtest/gtest.h>
#include <memory_resource>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

TEST(RenderSorting, SameMaterialLayerAndOrderOverrideDepthOnBothDrainPaths)
{
    std::vector<SortedTransparentRecord> records(3);
    records[0].ViewDepth = 1;
    records[1].ViewDepth = 100;
    records[2].ViewDepth = 50;
    std::vector<RenderSortKey> keys(3);
    keys[0].Layer = -1;
    keys[0].Depth = 1;
    keys[1].Order = 10;
    keys[1].Depth = 100;
    keys[2].Order = -10;
    keys[2].Depth = 50;
    SortedTransparentRunPartition partition;
    std::pmr::monotonic_buffer_resource scratch;
    BuildAuthoredTransparentRunPartition(records, keys, scratch, partition);
    const auto order = BuildSortedTransparentCpuOrder(records, partition);
    EXPECT_EQ(order, (std::vector<uint32_t>{0, 2, 1}));
    ASSERT_EQ(partition.RunCount.size(), 3u);
    for (size_t i = 1; i < order.size(); ++i)
    {
        const auto before = order[i - 1], after = order[i];
        EXPECT_TRUE(SortedTransparentKeyLess(
            MakeSortedTransparentKey(records[before].ViewDepth, static_cast<uint16_t>(records[before].RunGroup), static_cast<uint16_t>(before)),
            MakeSortedTransparentKey(records[after].ViewDepth, static_cast<uint16_t>(records[after].RunGroup), static_cast<uint16_t>(after))));
    }
}

TEST(RenderSorting, AlternatingMaterialsRemainInterleavedBeyondGpuCapacity)
{
    const uint32_t count = kSortedTransparentSortCapacity + 1;
    std::vector<SortedTransparentRecord> records(count);
    std::vector<RenderSortKey> keys(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        records[i].RunGroup = i % 2;
        records[i].ViewDepth = static_cast<float>(i);
        keys[i].Order = static_cast<int32>(i);
        keys[i].Depth = records[i].ViewDepth;
    }
    SortedTransparentRunPartition partition;
    std::pmr::monotonic_buffer_resource scratch;
    BuildAuthoredTransparentRunPartition(records, keys, scratch, partition);
    const auto order = BuildSortedTransparentCpuOrder(records, partition);
    ASSERT_EQ(partition.RunCount.size(), count);
    for (uint32_t i = 0; i < count; ++i)
    {
        EXPECT_EQ(order[i], i);
        EXPECT_EQ(partition.GroupOfSlot[i], i % 2);
    }
}

TEST(RenderSorting, ParentGroupUsesViewDepthAndKeepsChildrenTogether)
{
    ECS::World world;
    auto parent = world.Create(Components::RenderSorting{}, Components::WorldTransform{});
    auto child = world.Create(Components::Parent{}, Components::RenderSorting{});
    auto sibling = world.Create(Components::Parent{}, Components::RenderSorting{});
    parent.Set(Components::RenderSorting{10, 3, true});
    auto transform = Components::WorldTransform{};
    transform.matrix[12] = 8;
    transform.matrix[14] = 200;
    parent.Set(transform);
    child.Set(Components::Parent{parent.GetHandle()});
    sibling.Set(Components::Parent{parent.GetHandle()});
    child.Set(Components::RenderSorting{40, 2, true});
    sibling.Set(Components::RenderSorting{-30, 1, false});
    world.ProcessCommands();
    const auto layers = Rendering::SortingLayers::Defaults();
    auto a = ResolveRenderSorting(world, child.GetHandle(), layers);
    auto b = ResolveRenderSorting(world, sibling.GetHandle(), layers);
    EXPECT_EQ(a.Group, parent.GetHandle());
    EXPECT_EQ(a.Layer, 10);
    const float position[]{1, 0, 0}, forward[]{1, 0, 0};
    auto keyA = MakeRenderSortKey(a, 100, position, forward);
    auto keyB = MakeRenderSortKey(b, 1, position, forward);
    EXPECT_EQ(keyA.Depth, 7);
    EXPECT_TRUE(RenderSortBefore(keyB, keyA));
    world.SetComponentEnabledImmediate(parent.GetHandle(), ECS::GetComponentTypeId<Components::RenderSorting>(), false);
    EXPECT_EQ(ResolveRenderSorting(world, child.GetHandle(), layers).Group, child.GetHandle());
}

TEST(RenderSorting, CustomLayerIdsAndPrioritiesRoundTripAndValidate)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({1234, "Characters", 15});
    nlohmann::json document;
    layers.WriteToJson(document);
    const auto loaded = Rendering::SortingLayers::FromJson(document);
    EXPECT_EQ(loaded.Layers, layers.Layers);
    EXPECT_EQ(loaded.Priority(1234), 15);
    EXPECT_EQ(loaded.Priority(9999), loaded.Priority(0));
    layers.Layers.push_back({1234, "Duplicate", 16});
    std::string error;
    EXPECT_FALSE(layers.Validate(error));
}

TEST(RenderSorting, MalformedLayerNumbersCannotWrapIntoValidIds)
{
    nlohmann::json document = {{"sortingLayers", nlohmann::json::array({
        {{"id", 0}, {"name", "Default"}, {"order", 0}},
        {{"id", uint64{4294967297}}, {"name", "Overflow"}, {"order", 1}}})}};
    EXPECT_EQ(Rendering::SortingLayers::FromJson(document).Layers,
              Rendering::SortingLayers::Defaults().Layers);
    document["sortingLayers"][1]["id"] = 1.5;
    EXPECT_EQ(Rendering::SortingLayers::FromJson(document).Layers,
              Rendering::SortingLayers::Defaults().Layers);
}

TEST(RenderSorting, PackagedPlayerRetainsCustomLayerIdentityAndPriority)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("sorting-game-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    GameConfig authored;
    authored.sortingLayers.Layers.push_back({1234, "Characters", 17});
    ASSERT_TRUE(SaveGameConfig(path, authored));
    const GameConfig loaded = LoadGameConfig(path);
    EXPECT_EQ(loaded.sortingLayers.Layers, authored.sortingLayers.Layers);
    EXPECT_EQ(loaded.sortingLayers.Priority(1234), 17);
    std::error_code error;
    std::filesystem::remove(path, error);
}

TEST(RenderSorting, DefaultComponentRetainsExistingMeshOrderingPath)
{
    ECS::World world;
    auto entity = world.Create(Components::RenderSorting{});
    world.ProcessCommands();
    const auto data = ResolveRenderSorting(world, entity.GetHandle(), Rendering::SortingLayers::Defaults());
    EXPECT_FALSE(data.Authored);
    EXPECT_FALSE(data.Grouped);
    EXPECT_EQ(data.Layer, 0);
    EXPECT_EQ(data.Order, 0);
}

TEST(RenderSorting, ContributorBetweenSameMaterialMeshesSplitsTheirIndirectRun)
{
    std::vector<SortedTransparentRecord> records(2);
    records[0].RunGroup = records[1].RunGroup = 0;
    std::vector<RenderSortKey> keys(2);
    keys[0].Order = 0;
    keys[1].Order = 3;
    std::vector<ECS::EntityHandle> entities(2);
    entities[0].id = 1;
    entities[1].id = 4;
    std::vector<SortedTransparentCommand> contributors(2);
    contributors[0].Sorting.Order = 2; // tile
    contributors[0].Entity.id = 3;
    contributors[1].Sorting.Order = 1; // sprite
    contributors[1].Entity.id = 2;
    SortedTransparentRunPartition partition;
    std::vector<uint32> meshOrder;
    std::vector<TransparentCompositionSlot> slots;
    std::pmr::monotonic_buffer_resource scratch;
    BuildTransparentComposition(records, keys, entities, contributors, scratch, partition, meshOrder, slots, false);
    ASSERT_EQ(slots.size(), 4u);
    EXPECT_FALSE(slots[0].Contributor);
    EXPECT_EQ(slots[0].Index, 0u);
    EXPECT_TRUE(slots[1].Contributor);
    EXPECT_EQ(slots[1].Index, 1u);
    EXPECT_TRUE(slots[2].Contributor);
    EXPECT_EQ(slots[2].Index, 0u);
    EXPECT_FALSE(slots[3].Contributor);
    EXPECT_EQ(slots[3].Index, 1u);
    EXPECT_EQ(partition.GroupOfSlot, (std::vector<uint32>{0, 0}));
    EXPECT_EQ(partition.RunCount, (std::vector<uint32>{1, 1}));
    EXPECT_EQ(meshOrder, (std::vector<uint32>{0, 1}));
}

TEST(RenderSorting, ContributorOnlyViewUsesDepthAndStableEntityTies)
{
    std::vector<SortedTransparentCommand> contributors(3);
    contributors[0].Sorting.Depth = 2;
    contributors[0].Entity.id = 8;
    contributors[1].Sorting.Depth = 10;
    contributors[1].Entity.id = 20;
    contributors[2].Sorting.Depth = 2;
    contributors[2].Entity.id = 3;
    SortedTransparentRunPartition partition;
    std::vector<uint32> meshOrder;
    std::vector<TransparentCompositionSlot> slots;
    std::pmr::monotonic_buffer_resource scratch;
    BuildTransparentComposition({}, {}, {}, contributors, scratch, partition, meshOrder, slots, false);
    ASSERT_EQ(slots.size(), 3u);
    EXPECT_EQ(slots[0].Index, 1u);
    EXPECT_EQ(slots[1].Index, 2u);
    EXPECT_EQ(slots[2].Index, 0u);
    EXPECT_TRUE(meshOrder.empty());
    EXPECT_TRUE(partition.RunCount.empty());
}

TEST(RenderSorting, DirectCompatibilityDrawsSplitDifferentIndexRanges)
{
    std::vector<SortedTransparentRecord> records(2);
    records[0].IndexCount = records[1].IndexCount = 6;
    records[1].FirstIndex = 6;
    std::vector<RenderSortKey> keys(2);
    std::vector<ECS::EntityHandle> entities(2);
    entities[0].id = 1;
    entities[1].id = 2;
    SortedTransparentRunPartition partition;
    std::vector<uint32> meshOrder;
    std::vector<TransparentCompositionSlot> slots;
    std::pmr::monotonic_buffer_resource scratch;
    BuildTransparentComposition(records, keys, entities, {}, scratch, partition, meshOrder, slots, true);
    EXPECT_EQ(partition.RunCount, (std::vector<uint32>{1, 1}));
    records[1].FirstIndex = 0;
    BuildTransparentComposition(records, keys, entities, {}, scratch, partition, meshOrder, slots, true);
    EXPECT_EQ(partition.RunCount, (std::vector<uint32>{2}));
}
