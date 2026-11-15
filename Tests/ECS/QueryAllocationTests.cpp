// The allocation contract of ECS query construction, measured rather than argued.
//
// Most query sites build the query as a temporary — world.Query<Read<A>, Write<B>>().Each(...)
// — and the per-frame systems among them build a fresh one every frame, many of them on pool
// workers. Everything a query derives from its type list is a compile-time constant, and the
// archetypes its filter matches are a list the World keeps current as archetypes come and go
// (ArchetypeMatchCache). So once a filter has run on a world, building and iterating a query
// with it allocates nothing, on any thread.
//
// No behavioural test can see this. A query that rebuilds its signatures, or copies its
// archetypes into a buffer of its own, on every construction returns exactly the same entities
// as one that reads the World's list; only an allocation count separates them.
//
// The parallel forks (Parallel, ParallelBatchEach, ParallelChunks) join before they return,
// so once warm they allocate nothing either: the functor stays on the caller's stack and
// each unit fits a task slab.
//
// The last two tests are not allocation measurements. They run queries on pool workers while
// the test thread creates archetypes those queries match, so the World's lists grow under
// readers that take no lock. They check counts in every build, and they check the publication
// order when built with ThreadSanitizer (the macos-arm64-local-tsan preset), like the worker
// test above.
//
// Its own target on purpose: the measurement is a process-wide allocation window, and
// ECSCoreTests is one binary of 28 suites. EntityDestroyNotificationTests is split out too:
// it arms allocation faults (Tests/ECS/CMakeLists.txt).

#include <gtest/gtest.h>

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "ArchetypeSpread.h"
#include "TestComponents.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/AllocationCountScope.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

// Enough archetypes that a per-construction match list has to grow if it is not reserved,
// and close to the archetype counts a loaded editor scene reaches.
constexpr unsigned kArchetypeCount = 200;
// The measured window runs many constructions so the per-construction figure is not a
// single sample.
constexpr std::size_t kMeasuredConstructions = 50;

std::size_t RunFreshQuery(World& world)
{
    std::size_t visited = 0;
    world.Query<Read<Position>, Write<Velocity>>().Each(
        [&visited](EntityHandle, const Position& position, Velocity& velocity)
        {
            velocity.x += position.x * 0.0f;
            ++visited;
        });
    return visited;
}
} // namespace

// The headline claim: a query built fresh, as the per-frame sites build theirs, allocates
// nothing once its filter has run on the world.
TEST(QueryAllocationTests, FreshConstructionAllocatesNothingOnceWarm)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    // The world has to have the shape the measurement claims, or the count means nothing.
    ASSERT_EQ(RunFreshQuery(world), static_cast<std::size_t>(kArchetypeCount));
    ASSERT_GE(world.GetArchetypeCount(), static_cast<std::size_t>(kArchetypeCount));

    // Settle: component registration, the per-instantiation signatures, the filter's match
    // list and the chunk version arrays are all first-use costs, and none of them is what
    // this pins.
    for (int warmup = 0; warmup < 3; ++warmup)
        RunFreshQuery(world);

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
            RunFreshQuery(world);
        allocations = probe.Count();
    }

    std::printf("[Alloc] fresh query construction + Each over %u archetypes: %zu allocations"
                " across %zu runs (%.2f per construction)\n",
                kArchetypeCount, allocations, kMeasuredConstructions,
                static_cast<double>(allocations) / kMeasuredConstructions);

    EXPECT_EQ(allocations, 0u)
        << "fresh query construction + Each allocated " << allocations << " times across "
        << kMeasuredConstructions << " runs ("
        << (static_cast<double>(allocations) / kMeasuredConstructions)
        << "/construction). Building the read, write or required signature per construction,"
           " or copying the matching archetypes into a buffer of the query's own, will do this.";
}

// A retained query — the shape GameUIHost's DocQueryCache uses — allocates nothing while
// the world is structurally unchanged: it reads its last snapshot of the World's list again,
// so the only way this count moves is an allocation on the iteration path itself.
TEST(QueryAllocationTests, RetainedQueryAllocatesNothingWithoutStructuralChange)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Query<Read<Position>, Write<Velocity>> query(&world);
    std::size_t visited = 0;
    const auto visit = [&visited](EntityHandle, const Position&, Velocity&) { ++visited; };

    query.Each(visit);
    ASSERT_EQ(visited, static_cast<std::size_t>(kArchetypeCount));

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
            query.Each(visit);
        allocations = probe.Count();
    }

    std::printf("[Alloc] retained query, %zu iterations of an unchanged world: %zu allocations\n",
                kMeasuredConstructions, allocations);

    EXPECT_EQ(allocations, 0u)
        << "a retained query allocated " << allocations << " times over "
        << kMeasuredConstructions
        << " iterations of an unchanged world. Each is allocating on the iteration path, or"
           " the query has started copying its archetypes again.";
}

// Turning an entity off and on again is an archetype move each way. Both edges are cached
// on the archetype after the first transition through them and the destination chunks already
// exist, so a steady-state toggle has nothing left to allocate — which is the claim that makes
// the enable model cheap enough to be the default everywhere.
TEST(QueryAllocationTests, TogglingEnableStateAllocatesNothingInSteadyState)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    const EntityHandle handle = entity.GetHandle();

    // Warm both edges and the destination chunks; the first transition through an edge
    // creates the archetype it names.
    for (int warmup = 0; warmup < 3; ++warmup)
    {
        Entity(&world, handle).SetEnabled(false);
        Entity(&world, handle).SetEnabled(true);
        Entity(&world, handle).SetEnabled<Velocity>(false);
        Entity(&world, handle).SetEnabled<Velocity>(true);
    }
    ASSERT_TRUE(Entity(&world, handle).IsEnabled());

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
        {
            Entity(&world, handle).SetEnabled(false);
            Entity(&world, handle).SetEnabled(true);
            Entity(&world, handle).SetEnabled<Velocity>(false);
            Entity(&world, handle).SetEnabled<Velocity>(true);
        }
        allocations = probe.Count();
    }

    // The toggles have to have done something, or a no-op would read as zero allocations.
    ASSERT_FALSE(Entity(&world, handle).Has<Disabled>());
    ASSERT_TRUE(Entity(&world, handle).IsEnabled<Velocity>());

    std::printf("[Alloc] %zu enable-state toggles (entity + component, both directions):"
                " %zu allocations\n",
                kMeasuredConstructions * 4, allocations);

    EXPECT_EQ(allocations, 0u)
        << "toggling enable state allocated " << allocations << " times across "
        << (kMeasuredConstructions * 4)
        << " transitions. An uncached archetype edge, a signature rebuilt per call, or a"
           " lifecycle buffer growing every frame will do this.";
}

// One entity switched off and on both ways across three swap windows; returns how many
// entities the disabled windows reported.
static std::size_t ToggleVelocityAcrossThreeWindows(World& world, EntityHandle handle)
{
    std::size_t reported = 0;
    Entity(&world, handle).SetEnabled(false);
    world.SwapLifecycleEvents();
    reported += world.GetDisabled<Velocity>().size();
    Entity(&world, handle).SetEnabled(true);
    Entity(&world, handle).SetEnabled<Velocity>(false);
    world.SwapLifecycleEvents();
    reported += world.GetDisabled<Velocity>().size();
    Entity(&world, handle).SetEnabled<Velocity>(true);
    world.SwapLifecycleEvents();
    return reported;
}

// A world whose Velocity consumers read GetDisabled<Velocity>() records the tag transitions in
// the lifecycle windows and builds the disabled window inside SwapLifecycleEvents. Both reuse
// their vectors frame to frame, so a frame of toggles and its swap allocate nothing once the
// buffers have grown to the frame's size.
TEST(QueryAllocationTests, SwappingDisabledEventsAllocatesNothingInSteadyState)
{
    World world(nullptr);
    world.EnableLifecycleEvents<Velocity>();
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    world.ProcessCommands();
    const EntityHandle handle = entity.GetHandle();

    for (int warmup = 0; warmup < 3; ++warmup)
        ToggleVelocityAcrossThreeWindows(world, handle);

    std::size_t allocations = 0;
    std::size_t reported = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
            reported += ToggleVelocityAcrossThreeWindows(world, handle);
        allocations = probe.Count();
    }

    // Both windows have to have reported the entity, or an empty build would read as zero.
    ASSERT_EQ(reported, kMeasuredConstructions * 2);

    std::printf("[Alloc] %zu frames of enable toggles with GetDisabled subscribed: %zu allocations\n",
                kMeasuredConstructions, allocations);
    EXPECT_EQ(allocations, 0u) << "building the disabled window allocated " << allocations << " times";
}

// The shape above is a root entity: its archetype carries neither activity tag, so both
// transitions are the cached activity pair. The engine produces two other shapes — a
// descendant of a disabled parent, which carries DisabledInHierarchy alone (written by the
// hierarchy pass), and an authored Disabled tag no pass has seen yet (scene load, the debug
// server) — and for those only Disabled moves. That single-tag transition resolves through
// the per-type add/remove edges, which cache both directions, so it is as free as the pair.
TEST(QueryAllocationTests, TogglingADescendantOfADisabledParentAllocatesNothingInSteadyState)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    const EntityHandle handle = entity.GetHandle();

    // What the hierarchy pass writes on a descendant of a disabled parent, re-applied after
    // every re-enable the way the pass would on its next update.
    world.AddComponentImmediate<DisabledInHierarchy>(handle, DisabledInHierarchy{});
    for (int warmup = 0; warmup < 3; ++warmup)
    {
        Entity(&world, handle).SetEnabled(false);
        Entity(&world, handle).SetEnabled(true);
        world.AddComponentImmediate<DisabledInHierarchy>(handle, DisabledInHierarchy{});
    }

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
        {
            Entity(&world, handle).SetEnabled(false);
            Entity(&world, handle).SetEnabled(true);
            world.AddComponentImmediate<DisabledInHierarchy>(handle, DisabledInHierarchy{});
        }
        allocations = probe.Count();
    }
    ASSERT_TRUE(Entity(&world, handle).IsEnabled());
    ASSERT_TRUE(Entity(&world, handle).Has<DisabledInHierarchy>());

    std::printf("[Alloc] %zu enable-state toggles on a descendant of a disabled parent:"
                " %zu allocations\n",
                kMeasuredConstructions * 2, allocations);

    EXPECT_EQ(allocations, 0u)
        << "toggling an entity that already carries DisabledInHierarchy allocated " << allocations
        << " times across " << (kMeasuredConstructions * 2)
        << " transitions: the single-tag transition is not going through the cached edges.";
}

TEST(QueryAllocationTests, ReEnablingAnAuthoredDisabledTagAllocatesNothingInSteadyState)
{
    World world(nullptr);
    PopulateArchetypeSpread(world, kArchetypeCount);

    Entity entity = world.Create();
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.0f, 0.0f, 0.0f});
    const EntityHandle handle = entity.GetHandle();

    // What scene load writes: the authored tag alone, before any hierarchy pass ran.
    world.AddComponentImmediate<Disabled>(handle, Disabled{});
    for (int warmup = 0; warmup < 3; ++warmup)
    {
        Entity(&world, handle).SetEnabled(true);
        world.AddComponentImmediate<Disabled>(handle, Disabled{});
    }

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
        {
            Entity(&world, handle).SetEnabled(true);
            world.AddComponentImmediate<Disabled>(handle, Disabled{});
        }
        allocations = probe.Count();
    }
    ASSERT_FALSE(Entity(&world, handle).IsEnabled());

    std::printf("[Alloc] %zu re-enables of an authored Disabled tag: %zu allocations\n",
                kMeasuredConstructions, allocations);

    EXPECT_EQ(allocations, 0u)
        << "re-enabling an entity that carries Disabled alone allocated " << allocations
        << " times across " << kMeasuredConstructions
        << " transitions: the single-tag transition is not going through the cached edges.";
}

namespace
{
// The parallel forks: several archetypes, several one-chunk batches in each, so every
// archetype's units are in flight under one Wait.
constexpr unsigned kForkArchetypeCount = 3;
constexpr std::size_t kForkEntitiesPerArchetype = 3000;
constexpr std::size_t kForkEntityCount = kForkArchetypeCount * kForkEntitiesPerArchetype;
constexpr std::size_t kForkMinBatchSize = 1;
// A chunk holds a few hundred of these entities, so each archetype spans several chunks.
constexpr std::size_t kForkMinChunksPerCall = kForkArchetypeCount * 4;
constexpr int kForkWarmupCalls = 100;
constexpr int kForkMeasuredCalls = 100;
constexpr int kForkCalls = kForkWarmupCalls + kForkMeasuredCalls;
constexpr std::size_t kForkWorkerCount = 4;

void PopulateForkArchetypes(World& world)
{
    for (unsigned mask = 0; mask < kForkArchetypeCount; ++mask)
    {
        for (std::size_t i = 0; i < kForkEntitiesPerArchetype; ++i)
        {
            auto entity = world.Create();
            entity.Set(Position{1.0f, 0.0f, 0.0f});
            entity.Set(Velocity{0.0f, 0.0f, 0.0f});
            AddArchetypeSpreadTags(entity, mask, std::make_index_sequence<kArchetypeSpreadTagCount>{});
        }
    }
    world.ProcessCommands();
}

// Warms the fork, then counts what kForkMeasuredCalls more calls allocate on every thread.
template <typename Fork>
std::uint64_t CountForkAllocationsAfterWarmup(const Fork& fork)
{
    for (int call = 0; call < kForkWarmupCalls; ++call)
        fork();
    const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
    for (int call = 0; call < kForkMeasuredCalls; ++call)
        fork();
    return probe.Count();
}
} // namespace

// A parallel fork joins before it returns, so neither its functor nor its units' shared
// state needs the heap: once the slab freelist and the tagged queue are warm, a retained
// query's fork allocates nothing on any thread.
TEST(QueryAllocationTests, ParallelAllocatesNothingAfterWarmup)
{
    JobSystem::WorkStealingThreadPool pool(kForkWorkerCount);
    World world(&pool);
    PopulateForkArchetypes(world);
    Query<Read<Position>, Write<Velocity>> query(&world);

    std::atomic<std::size_t> visited{0};
    const auto fork = [&]
    {
        query.Parallel(
            [&visited](EntityHandle, const Position& position, Velocity& velocity)
            {
                velocity.x += position.x;
                visited.fetch_add(1, std::memory_order_relaxed);
            },
            kForkMinBatchSize);
    };
    const std::uint64_t allocations = CountForkAllocationsAfterWarmup(fork);

    ASSERT_EQ(visited.load(), kForkEntityCount * kForkCalls);
    std::printf("[Alloc] %d Parallel calls after warm-up: %llu allocations\n", kForkMeasuredCalls,
                static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u);
}

TEST(QueryAllocationTests, ParallelBatchEachAllocatesNothingAfterWarmup)
{
    JobSystem::WorkStealingThreadPool pool(kForkWorkerCount);
    World world(&pool);
    PopulateForkArchetypes(world);
    Query<Read<Position>, Write<Velocity>> query(&world);

    std::atomic<std::size_t> visited{0};
    std::atomic<std::size_t> chunks{0};
    const auto fork = [&]
    {
        query.ParallelBatchEach(
            [&visited, &chunks](const Position* positions, Velocity* velocities, std::size_t count)
            {
                for (std::size_t i = 0; i < count; ++i)
                    velocities[i].x += positions[i].x;
                visited.fetch_add(count, std::memory_order_relaxed);
                chunks.fetch_add(1, std::memory_order_relaxed);
            },
            kForkMinBatchSize);
    };
    const std::uint64_t allocations = CountForkAllocationsAfterWarmup(fork);

    ASSERT_EQ(visited.load(), kForkEntityCount * kForkCalls);
    ASSERT_GE(chunks.load(), kForkMinChunksPerCall * kForkCalls);
    std::printf("[Alloc] %d ParallelBatchEach calls after warm-up: %llu allocations\n",
                kForkMeasuredCalls, static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u);
}

TEST(QueryAllocationTests, ParallelChunksAllocatesNothingAfterWarmup)
{
    JobSystem::WorkStealingThreadPool pool(kForkWorkerCount);
    World world(&pool);
    PopulateForkArchetypes(world);
    Query<Read<Position>, Write<Velocity>> query(&world);

    std::atomic<std::size_t> visited{0};
    std::atomic<std::size_t> chunks{0};
    const auto fork = [&]
    {
        query.ParallelChunks(
            [&visited, &chunks](const Position* positions, Velocity* velocities, std::size_t count)
            {
                for (std::size_t i = 0; i < count; ++i)
                    velocities[i].x += positions[i].x;
                visited.fetch_add(count, std::memory_order_relaxed);
                chunks.fetch_add(1, std::memory_order_relaxed);
            });
    };
    const std::uint64_t allocations = CountForkAllocationsAfterWarmup(fork);

    ASSERT_EQ(visited.load(), kForkEntityCount * kForkCalls);
    ASSERT_GE(chunks.load(), kForkMinChunksPerCall * kForkCalls);
    std::printf("[Alloc] %d ParallelChunks calls after warm-up: %llu allocations\n",
                kForkMeasuredCalls, static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u);
}

namespace
{
// Visits the entities whose archetype carries ArchetypeTag<Bit>, through a temporary: one
// distinct filter per Bit. Read-only, so units on several threads can share the entities.
template <int Bit>
std::size_t CountTagged(World& world)
{
    std::size_t visited = 0;
    world.Query<Read<Position>, Read<ArchetypeTag<Bit>>>().Each(
        [&visited](const Position&, const ArchetypeTag<Bit>&) { ++visited; });
    return visited;
}

using CountTaggedFunction = std::size_t (*)(World&);

template <std::size_t... Bits>
constexpr std::array<CountTaggedFunction, sizeof...(Bits)> MakeCountTagged(std::index_sequence<Bits...>)
{
    return {&CountTagged<static_cast<int>(Bits)>...};
}

constexpr auto kCountTagged = MakeCountTagged(std::make_index_sequence<kArchetypeSpreadTagCount>{});

// The entities of a spread over `archetypeCount` masks that carry tag `bit`.
std::size_t ExpectedTagged(unsigned archetypeCount, std::size_t bit)
{
    std::size_t expected = 0;
    for (unsigned mask = 0; mask < archetypeCount; ++mask)
        expected += (mask >> bit) & 1u;
    return expected;
}

// The filter every unit shares, read-only for the same reason.
std::size_t CountMoving(World& world)
{
    std::size_t visited = 0;
    world.Query<Read<Position>, Read<Velocity>>().Each(
        [&visited](const Position&, const Velocity&) { ++visited; });
    return visited;
}

// Enough units that every tag filter is first used by several of them at once.
constexpr std::size_t kWorkerQueryUnits = 64;
} // namespace

// Queries are built on pool workers: a wave's systems run there, and fork units build their
// own. The first round registers the tag filters from whichever threads reach them first, and
// every unit looks up one shared filter; once warm, no thread allocates.
TEST(QueryAllocationTests, QueriesBuiltOnWorkersAllocateNothingOnceWarm)
{
    JobSystem::WorkStealingThreadPool pool(kForkWorkerCount);
    World world(&pool);
    PopulateArchetypeSpread(world, kArchetypeCount);

    std::array<std::size_t, kWorkerQueryUnits> tagged{};
    std::array<std::size_t, kWorkerQueryUnits> moving{};
    const auto runUnit = [&world, &tagged, &moving](std::size_t unit)
    {
        tagged[unit] = kCountTagged[unit % kCountTagged.size()](world);
        moving[unit] = CountMoving(world);
    };
    const auto runAllUnits = [&pool, &runUnit] { JobSystem::ParallelFor(&pool, kWorkerQueryUnits, runUnit); };

    runAllUnits();
    for (std::size_t unit = 0; unit < kWorkerQueryUnits; ++unit)
    {
        ASSERT_EQ(tagged[unit], ExpectedTagged(kArchetypeCount, unit % kCountTagged.size())) << "unit " << unit;
        ASSERT_EQ(moving[unit], static_cast<std::size_t>(kArchetypeCount)) << "unit " << unit;
    }

    for (int warmup = 0; warmup < 3; ++warmup)
        runAllUnits();

    std::uint64_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        for (std::size_t run = 0; run < kMeasuredConstructions; ++run)
            runAllUnits();
        allocations = probe.Count();
    }

    std::printf("[Alloc] %zu rounds of %zu worker units (2 temporaries each): %llu allocations\n",
                kMeasuredConstructions, kWorkerQueryUnits, static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u)
        << "queries built on pool workers allocated " << allocations << " times across "
        << kMeasuredConstructions * kWorkerQueryUnits * 2 << " constructions";
}

// Positive control. A probe that never counts would make the arms above pass whatever the
// query does, so prove the allocation counter sees a path that must allocate.
TEST(QueryAllocationTests, TheProbeSeesTheAllocationsOfEntityCreation)
{
    World world(nullptr);

    std::size_t allocations = 0;
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
        PopulateArchetypeSpread(world, kArchetypeCount);
        allocations = probe.Count();
    }

    EXPECT_GT(allocations, 0u)
        << "the allocation probe counted nothing while an empty world grew "
        << kArchetypeCount
        << " archetypes — it is not wired up, so the arms above prove nothing";
}

namespace
{
// Concurrent growth. The test thread creates archetypes that match Read<Position> while reader
// tasks on pool workers iterate queries over the same world: a fresh temporary, a retained query
// every reader shares, a retained query of the reader's own, and the first use of a filter. Each
// reader counts what it saw that the world never held.

template <int N>
struct GrowthTag
{
    int32 Value = 0;
};

// The Read<Position> list starts with the seed archetype and replaces its buffer three times
// (at 8, 16 and 32 entries) while the readers run.
constexpr std::size_t kGrowthArchetypeCount = 40;
constexpr std::size_t kGrowthSeedEntities = 16;
// Every entity the tests create carries this Position.x; a visited entity without it was read
// before its creating call wrote it.
constexpr float kGrowthPositionX = 42.0f;
constexpr std::size_t kGrowthReaderCount = 4;
constexpr int kGrowthRounds = 4;
// A retained query re-reads its last snapshot without a lock while the structural version stands.
// Most of a reader's time goes to these reads: ThreadSanitizer checks a list's publication only
// against a read made after the list grows and before the reader next takes the World's lock.
constexpr int kRetainedRereads = 512;
// Gap between two archetype creations, so the readers run between every pair.
constexpr std::chrono::microseconds kGrowthCreationGap{200};
constexpr std::chrono::seconds kReaderStartTimeout{10};

enum class GrowthShape
{
    // ReserveArchetypeCapacity(0): the lists grow, and no entity is placed.
    EmptyArchetypes,
    // CreateHandle: each archetype is created with its first entity, as a system's immediate
    // structural change does, so a list gains it before its entity is written.
    PopulatedArchetypes,
};

using GrowthFunction = void (*)(World&);
using GrowthCountFunction = std::size_t (*)(World&);

template <std::size_t N>
void ReserveEmptyGrowthArchetype(World& world)
{
    world.ReserveArchetypeCapacity<Position, GrowthTag<static_cast<int>(N)>>(0);
}

template <std::size_t N>
void CreateGrowthEntity(World& world)
{
    world.CreateHandle(Position{kGrowthPositionX, 0.0f, 0.0f}, GrowthTag<static_cast<int>(N)>{});
}

// One distinct filter per N, so its first use registers it while the lists grow.
template <std::size_t N>
std::size_t CountGrowthTagged(World& world)
{
    return world.Query<Read<Position>, Read<GrowthTag<static_cast<int>(N)>>>().Count();
}

template <std::size_t... Ns>
constexpr std::array<GrowthFunction, sizeof...(Ns)> MakeReserveEmptyGrowthArchetypes(std::index_sequence<Ns...>)
{
    return {&ReserveEmptyGrowthArchetype<Ns>...};
}

template <std::size_t... Ns>
constexpr std::array<GrowthFunction, sizeof...(Ns)> MakeCreateGrowthEntities(std::index_sequence<Ns...>)
{
    return {&CreateGrowthEntity<Ns>...};
}

template <std::size_t... Ns>
constexpr std::array<GrowthCountFunction, sizeof...(Ns)> MakeCountGrowthTagged(std::index_sequence<Ns...>)
{
    return {&CountGrowthTagged<Ns>...};
}

constexpr auto kReserveEmptyGrowthArchetypes =
    MakeReserveEmptyGrowthArchetypes(std::make_index_sequence<kGrowthArchetypeCount>{});
constexpr auto kCreateGrowthEntities = MakeCreateGrowthEntities(std::make_index_sequence<kGrowthArchetypeCount>{});
constexpr auto kCountGrowthTagged = MakeCountGrowthTagged(std::make_index_sequence<kGrowthArchetypeCount>{});

// Component registration is process-wide and not what these tests race: do it up front.
void RegisterGrowthComponents()
{
    World scratch(nullptr);
    for (GrowthFunction create : kCreateGrowthEntities)
        create(scratch);
    for (GrowthCountFunction count : kCountGrowthTagged)
        count(scratch);
}

struct GrowthRound
{
    World* GrowingWorld = nullptr;
    // One query object iterated by every reader at once.
    Query<Read<Position>>* Shared = nullptr;
    GrowthShape Shape = GrowthShape::EmptyArchetypes;
    std::atomic<std::size_t> StartedReaders{0};
    std::atomic<bool> Done{false};
};

struct GrowthReaderTally
{
    std::size_t Iterations = 0;
    std::size_t Anomalies = 0;
};

// A count of Read<Position> entities the world held at some moment of the round.
bool IsCountTheWorldHeld(std::size_t count, GrowthShape shape)
{
    if (shape == GrowthShape::EmptyArchetypes)
        return count == kGrowthSeedEntities;
    return count >= kGrowthSeedEntities && count <= kGrowthSeedEntities + kGrowthArchetypeCount;
}

void ReadWhileArchetypesAreCreated(GrowthRound& round, std::size_t reader, GrowthReaderTally& tally)
{
    World& world = *round.GrowingWorld;
    Query<Read<Position>> own(&world);
    std::size_t lastOwnCount = 0;
    const std::size_t maxTagged = round.Shape == GrowthShape::EmptyArchetypes ? 0 : 1;
    round.StartedReaders.fetch_add(1, std::memory_order_acq_rel);
    while (!round.Done.load(std::memory_order_acquire))
    {
        std::size_t visited = 0;
        std::size_t unwritten = 0;
        world.Query<Read<Position>>().Each(
            [&visited, &unwritten](const Position& position)
            {
                ++visited;
                if (position.x != kGrowthPositionX)
                    ++unwritten;
            });
        if (!IsCountTheWorldHeld(visited, round.Shape) || unwritten != 0)
            ++tally.Anomalies;

        for (int reread = 0; reread < kRetainedRereads; ++reread)
        {
            const std::size_t ownCount = own.Count();
            // Archetypes are only added, so a query that one thread iterates never counts fewer.
            if (!IsCountTheWorldHeld(ownCount, round.Shape) || ownCount < lastOwnCount ||
                !IsCountTheWorldHeld(round.Shared->Count(), round.Shape))
                ++tally.Anomalies;
            lastOwnCount = ownCount;
        }

        const std::size_t filter = (tally.Iterations * kGrowthReaderCount + reader) % kCountGrowthTagged.size();
        if (kCountGrowthTagged[filter](world) > maxTagged)
            ++tally.Anomalies;
        ++tally.Iterations;
    }
}

bool WaitForReadersToStart(const GrowthRound& round)
{
    const auto deadline = std::chrono::steady_clock::now() + kReaderStartTimeout;
    while (round.StartedReaders.load(std::memory_order_acquire) < kGrowthReaderCount)
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

void CreateGrowthArchetypes(World& world, GrowthShape shape)
{
    const auto& create = shape == GrowthShape::EmptyArchetypes ? kReserveEmptyGrowthArchetypes : kCreateGrowthEntities;
    for (GrowthFunction function : create)
    {
        function(world);
        std::this_thread::sleep_for(kGrowthCreationGap);
    }
}

// One round on a fresh world: the readers start, the test thread creates every growth
// archetype, the readers stop, and the tallies and the final counts are checked. Adds the
// readers' iterations to `readerIterations`.
void RunGrowthRound(GrowthShape shape, std::size_t& readerIterations)
{
    JobSystem::WorkStealingThreadPool pool(kGrowthReaderCount);
    World world(nullptr);
    for (std::size_t seed = 0; seed < kGrowthSeedEntities; ++seed)
        world.CreateHandle(Position{kGrowthPositionX, 0.0f, 0.0f});
    Query<Read<Position>> shared(&world);
    ASSERT_EQ(shared.Count(), kGrowthSeedEntities);

    GrowthRound round;
    round.GrowingWorld = &world;
    round.Shared = &shared;
    round.Shape = shape;
    std::array<GrowthReaderTally, kGrowthReaderCount> tallies{};
    std::vector<JobSystem::TaskHandle> readers;
    for (std::size_t reader = 0; reader < kGrowthReaderCount; ++reader)
        readers.push_back(pool.Submit([&round, reader, &tallies]
                                      { ReadWhileArchetypesAreCreated(round, reader, tallies[reader]); }));

    const bool started = WaitForReadersToStart(round);
    if (started)
        CreateGrowthArchetypes(world, shape);
    round.Done.store(true, std::memory_order_release);
    for (JobSystem::TaskHandle& reader : readers)
        reader.Wait();
    ASSERT_TRUE(started) << "the reader tasks did not all start within " << kReaderStartTimeout.count() << " s";

    for (std::size_t reader = 0; reader < kGrowthReaderCount; ++reader)
    {
        readerIterations += tallies[reader].Iterations;
        EXPECT_GT(tallies[reader].Iterations, 0u) << "reader " << reader;
        EXPECT_EQ(tallies[reader].Anomalies, 0u) << "reader " << reader << " of " << tallies[reader].Iterations
                                                 << " iterations";
    }
    const std::size_t expected =
        kGrowthSeedEntities + (shape == GrowthShape::EmptyArchetypes ? 0 : kGrowthArchetypeCount);
    EXPECT_EQ(shared.Count(), expected);
    EXPECT_EQ(world.Query<Read<Position>>().Count(), expected);
}
} // namespace

// The lists replace their buffers while queries on other threads read them with no lock: a
// fresh temporary, a retained query shared by every reader, one per reader, and filters
// registered for the first time. Empty archetypes, so the readers touch only the lists.
TEST(QueryConcurrentGrowthTests, ListsGrowingUnderReadersOnOtherThreadsKeepWhatTheyHeld)
{
    RegisterGrowthComponents();
    std::size_t readerIterations = 0;
    for (int round = 0; round < kGrowthRounds; ++round)
        RunGrowthRound(GrowthShape::EmptyArchetypes, readerIterations);
    std::printf("[Growth] %d rounds of %zu empty archetypes: %zu reader iterations\n", kGrowthRounds,
                kGrowthArchetypeCount, readerIterations);
}

// Each new archetype is created with its first entity while the same readers run. A list gains
// the archetype before the entity is placed, so a query that read the list past the count it
// took under the World's lock would visit an archetype still being filled.
TEST(QueryConcurrentGrowthTests, QueriesOnOtherThreadsVisitAnArchetypeOnlyOnceItsCreationFinished)
{
    RegisterGrowthComponents();
    std::size_t readerIterations = 0;
    for (int round = 0; round < kGrowthRounds; ++round)
        RunGrowthRound(GrowthShape::PopulatedArchetypes, readerIterations);
    std::printf("[Growth] %d rounds of %zu populated archetypes: %zu reader iterations\n", kGrowthRounds,
                kGrowthArchetypeCount, readerIterations);
}
