// The bookkeeping of ArchetypeMatchCache, checked list by list: which archetypes a registered
// filter holds as archetypes are created and removed, that filters sharing a cache key keep lists
// of their own, and that a snapshot taken before a list grows still reads what it read. The
// World-level behaviour (queries across archetype creation, pruning and Clear) is in
// QueryInvalidationAndOverflowTest.cpp.

#include <gtest/gtest.h>

#include "ECS/Archetype/ArchetypeMatchCache.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Types/FlatSet.h"
#include "ArchetypeSpread.h"
#include "TestComponents.h"

#include <algorithm>
#include <initializer_list>
#include <span>
#include <vector>

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace
{
// The archetype of a fresh entity carrying exactly `Ts`.
template <typename... Ts>
Archetype* ArchetypeWith(World& world)
{
    Entity entity = world.Create();
    (entity.Set(Ts{}), ...);
    world.ProcessCommands();
    return world.GetEntityArchetype(entity.GetHandle());
}

std::vector<Archetype*> Sorted(std::span<Archetype* const> archetypes)
{
    std::vector<Archetype*> sorted(archetypes.begin(), archetypes.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

std::vector<Archetype*> SortedOf(std::initializer_list<Archetype*> archetypes)
{
    return Sorted(std::span<Archetype* const>(archetypes.begin(), archetypes.size()));
}

ArchetypeFilter RequiringPosition()
{
    ArchetypeFilter filter;
    filter.Required = &SignatureOf<Position>();
    return filter;
}

ComponentSignature SignatureOfId(ComponentTypeId id)
{
    ComponentSignature signature;
    signature.Add(id);
    return signature;
}
} // namespace

TEST(ArchetypeMatchCacheTest, AListHoldsExactlyTheMatchingArchetypesAsTheyComeAndGo)
{
    World world(nullptr);
    Archetype* position = ArchetypeWith<Position>(world);
    Archetype* moving = ArchetypeWith<Position, Velocity>(world);
    Archetype* velocity = ArchetypeWith<Velocity>(world);

    ArchetypeMatchCache cache;
    for (Archetype* archetype : {position, moving, velocity})
        cache.AddArchetype(archetype);

    const ArchetypeMatchList& list = cache.Add(RequiringPosition());
    EXPECT_EQ(Sorted(list.GetArchetypes()), SortedOf({position, moving}));

    // A filter that differs only by an exclusion is another list.
    ArchetypeFilter stillPosition = RequiringPosition();
    stillPosition.Exclude = &SignatureOf<Velocity>();
    EXPECT_EQ(cache.Find(stillPosition), nullptr);
    EXPECT_EQ(cache.Find(RequiringPosition()), &list);

    Archetype* healthy = ArchetypeWith<Position, Health>(world);
    cache.AddArchetype(healthy);
    EXPECT_EQ(Sorted(list.GetArchetypes()), SortedOf({position, moving, healthy}));

    GameEngine::FlatSet<Archetype*> removed;
    removed.Insert(moving);
    cache.RemoveArchetypes(removed);
    EXPECT_EQ(Sorted(list.GetArchetypes()), SortedOf({position, healthy}));

    // Clear drops the archetypes and keeps the registration.
    cache.Clear();
    EXPECT_TRUE(list.GetArchetypes().empty());
    EXPECT_EQ(cache.Find(RequiringPosition()), &list);
}

// Two filters whose keys collide share one cache entry, chained. ComponentSignature::Hash() keeps
// 0 for "not computed yet" and stores a hash that computes to 0 as 1, and a one-id signature
// hashes to its id plus a constant, so the id whose hash computes to 0 and the id after it both
// hash to 1. Filters that differ only in excluding one or the other share a key. The ids name no
// component; they only build the filters.
TEST(ArchetypeMatchCacheTest, FiltersThatShareACacheKeyKeepListsOfTheirOwn)
{
    World world(nullptr);
    Archetype* position = ArchetypeWith<Position>(world);
    Archetype* moving = ArchetypeWith<Position, Velocity>(world);

    const ComponentTypeId hashesToZero = ComponentTypeId{0} - SignatureOfId(0).Hash();
    const ComponentSignature foldedToOne = SignatureOfId(hashesToZero);
    const ComponentSignature hashesToOne = SignatureOfId(hashesToZero + 1);
    ASSERT_EQ(foldedToOne.Hash(), hashesToOne.Hash())
        << "ComponentSignature::Hash() changed: pick two ids whose one-id signatures hash alike";

    ArchetypeFilter excludingFirst = RequiringPosition();
    excludingFirst.Exclude = &foldedToOne;
    ArchetypeFilter excludingSecond = RequiringPosition();
    excludingSecond.Exclude = &hashesToOne;

    ArchetypeMatchCache cache;
    cache.AddArchetype(position);
    const ArchetypeMatchList& first = cache.Add(excludingFirst);
    EXPECT_EQ(cache.Find(excludingSecond), nullptr);
    const ArchetypeMatchList& second = cache.Add(excludingSecond);
    EXPECT_NE(&first, &second);
    EXPECT_EQ(cache.Find(excludingFirst), &first);
    EXPECT_EQ(cache.Find(excludingSecond), &second);

    // Every list on the shared key follows the archetypes, not only the one registered last.
    cache.AddArchetype(moving);
    EXPECT_EQ(Sorted(first.GetArchetypes()), SortedOf({position, moving}));
    EXPECT_EQ(Sorted(second.GetArchetypes()), SortedOf({position, moving}));

    GameEngine::FlatSet<Archetype*> removed;
    removed.Insert(position);
    cache.RemoveArchetypes(removed);
    EXPECT_EQ(Sorted(first.GetArchetypes()), SortedOf({moving}));
    EXPECT_EQ(Sorted(second.GetArchetypes()), SortedOf({moving}));
}

// A reader on another thread may hold a snapshot while an archetype is appended. Growth copies
// into a new buffer and keeps the old one, so the snapshot still reads its own entries.
TEST(ArchetypeMatchCacheTest, ASnapshotTakenBeforeTheListGrowsStillReadsItsEntries)
{
    constexpr unsigned kArchetypeCount = 64;
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);
    std::vector<Archetype*> archetypes = world.GetAllArchetypes();
    std::erase_if(archetypes, [](const Archetype* archetype)
                  { return !archetype->GetSignature().Contains(GetComponentTypeId<Position>()); });

    ArchetypeMatchCache cache;
    const ArchetypeMatchList& list = cache.Add(RequiringPosition());
    std::vector<std::span<Archetype* const>> snapshots;
    for (Archetype* archetype : archetypes)
    {
        cache.AddArchetype(archetype);
        snapshots.push_back(list.GetArchetypes());
    }

    ASSERT_EQ(list.GetArchetypes().size(), archetypes.size());
    for (std::size_t snapshot = 0; snapshot < snapshots.size(); ++snapshot)
    {
        ASSERT_EQ(snapshots[snapshot].size(), snapshot + 1);
        for (std::size_t entry = 0; entry <= snapshot; ++entry)
            EXPECT_EQ(snapshots[snapshot][entry], archetypes[entry]) << "snapshot " << snapshot << ", entry " << entry;
    }
}

TEST(ArchetypeMatchCacheTest, PendingRequiredSchemasStayHidden)
{
    World world;
    Archetype* published = ArchetypeWith<Position>(world);
    world.RegisterComponents<PendingActivation>();
    const ComponentSignature pendingSignature = Detail::BuildSignature<Position, PendingActivation>();
    Archetype* pending = world.GetOrCreateArchetype(pendingSignature);

    ArchetypeMatchCache cache;
    cache.AddArchetype(pending);
    const ComponentSignature required = Detail::BuildSignature<Position>();
    const ArchetypeFilter filter{&required};
    const ArchetypeMatchList& matches = cache.Add(filter);
    EXPECT_TRUE(matches.GetArchetypes().empty());
    cache.AddArchetype(published);
    ASSERT_EQ(matches.GetArchetypes().size(), 1u);
    EXPECT_EQ(matches.GetArchetypes()[0], published);
}
