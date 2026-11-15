#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
// Test-owned components: the .cpp that touches them instantiates the World
// component templates itself (see ECS/ECSTemplates.h).
#include "ECS/ECSTemplates.h"
#include "TestComponents.h"
#include "ECS/Component/ComponentRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <cstddef>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS::test;
using namespace GameEngine::ECS;

class QueryInvalidationAndOverflowTest : public ::testing::Test {
protected:
    void SetUp() override {
        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        world = std::make_unique<World>(jobSystem.get());
    }
    void TearDown() override {
        world.reset();
        jobSystem.reset();
        ComponentRegistry::Clear();
    }
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<World> world;
};

TEST_F(QueryInvalidationAndOverflowTest, QueryCacheUpdatesOnStructuralChange) {
    // Initial entities with Position only
    std::vector<Entity> entities;
    for (int i = 0; i < 5; ++i) {
        auto e = world->Create();
        e.Set(Position{(float)i, 0, 0});
        entities.push_back(e);
    }
    world->ProcessCommands();

    // Build query for Position,Velocity and ensure zero count initially
    auto q = world->Query<Read<Position>, Read<Velocity>>();
    EXPECT_EQ(q.Count(), 0u);

    // Add Velocity to some entities (structural change to Position+Velocity archetype)
    for (int i = 0; i < 3; ++i) {
        entities[i].Set(Velocity{1,0,0});
    }
    world->ProcessCommands();

    // Query should now see updated archetypes without explicit invalidation
    size_t found = 0;
    q.Each([&](EntityHandle, const Position& p, const Velocity& v){ (void)p; (void)v; ++found; });
    EXPECT_EQ(found, 3u);
}

TEST_F(QueryInvalidationAndOverflowTest, CommandOverflowFallsBackToImmediate) {
    // Create a world with extremely small command buffers by reinitializing with tiny config
    WorldConfig cfg;
    cfg.CommandBufferSize = 2; // force overflow quickly
    World tinyWorld(cfg, jobSystem.get());

    // Create more commands than buffer capacity without processCommands in-between
    auto e1 = tinyWorld.Create();
    for (int i = 0; i < 10; ++i) {
        e1.Set(Position{(float)i, 0, 0});
    }
    // Some commands should have fallen back immediately; processing should still be safe
    tinyWorld.ProcessCommands();

    // Component must exist regardless of overflow
    auto* pos = e1.Get<Position>();
    ASSERT_NE(pos, nullptr);
}

namespace {
// Entities a fresh Query<Read<Position>> visits, built as a per-frame system builds it.
std::size_t CountPositions(World& world) {
    std::size_t visited = 0;
    world.Query<Read<Position>>().Each([&visited](const Position&) { ++visited; });
    return visited;
}

std::size_t CountPositions(Query<Read<Position>>& query) {
    std::size_t visited = 0;
    query.Each([&visited](const Position&) { ++visited; });
    return visited;
}
} // namespace

// The World keeps a filter's archetypes current once a query with it has run: an archetype
// created later is visited by the next temporary and by a query object kept across frames.
TEST_F(QueryInvalidationAndOverflowTest, ArchetypeCreatedAfterAFilterRanIsVisited) {
    world->Create().Set(Position{1, 0, 0});
    world->ProcessCommands();
    Query<Read<Position>> retained(world.get());
    ASSERT_EQ(CountPositions(*world), 1u);
    ASSERT_EQ(CountPositions(retained), 1u);

    const std::size_t archetypesBefore = world->GetArchetypeCount();
    world->Create().Set(Position{2, 0, 0}).Set(Velocity{1, 0, 0});
    world->ProcessCommands();
    ASSERT_GT(world->GetArchetypeCount(), archetypesBefore) << "the second entity must land in a new archetype";

    EXPECT_EQ(CountPositions(*world), 2u);
    EXPECT_EQ(CountPositions(retained), 2u);
    EXPECT_EQ(retained.Count(), 2u);
}

// Pruning destroys the empty archetypes. A query that ran before the prune visits only what
// is left, and an archetype re-created with the same components afterwards is visited once.
TEST_F(QueryInvalidationAndOverflowTest, PrunedArchetypeIsNotVisited) {
    Entity still = world->Create();
    still.Set(Position{1, 0, 0});
    Entity moving = world->Create();
    moving.Set(Position{2, 0, 0}).Set(Velocity{1, 0, 0});
    world->ProcessCommands();
    Query<Read<Position>> retained(world.get());
    ASSERT_EQ(CountPositions(retained), 2u);

    world->DestroyEntityImmediate(moving.GetHandle());
    ASSERT_GE(world->PruneEmptyArchetypes(), 1u);

    std::vector<EntityHandle> visited;
    retained.Each([&visited](EntityHandle entity, const Position&) { visited.push_back(entity); });
    ASSERT_EQ(visited.size(), 1u);
    EXPECT_EQ(visited[0], still.GetHandle());
    EXPECT_EQ(CountPositions(*world), 1u);

    world->Create().Set(Position{3, 0, 0}).Set(Velocity{1, 0, 0});
    world->ProcessCommands();
    EXPECT_EQ(CountPositions(retained), 2u);
    EXPECT_EQ(CountPositions(*world), 2u);
}

// Clear drops every archetype; the queries that ran before see only what is created after it.
TEST_F(QueryInvalidationAndOverflowTest, QueryAfterClearSeesOnlyNewEntities) {
    world->Create().Set(Position{1, 0, 0});
    world->Create().Set(Position{2, 0, 0}).Set(Velocity{1, 0, 0});
    world->ProcessCommands();
    Query<Read<Position>> retained(world.get());
    ASSERT_EQ(CountPositions(retained), 2u);

    world->Clear();
    EXPECT_EQ(CountPositions(retained), 0u);
    EXPECT_EQ(CountPositions(*world), 0u);

    world->Create().Set(Position{3, 0, 0}).Set(Velocity{1, 0, 0});
    world->ProcessCommands();
    EXPECT_EQ(CountPositions(retained), 1u);
    EXPECT_EQ(CountPositions(*world), 1u);
}

// An iteration visits the archetypes that existed when it started: one created by its own
// callback is left for the next iteration, as with any structural change made mid-iteration.
TEST_F(QueryInvalidationAndOverflowTest, ArchetypeCreatedDuringEachIsVisitedByTheNextOne) {
    world->Create().Set(Position{1, 0, 0});
    world->ProcessCommands();

    std::size_t visited = 0;
    world->Query<Read<Position>>().Each([this, &visited](const Position&) {
        if (visited++ == 0)
            world->CreateHandle(Position{2, 0, 0}, Velocity{1, 0, 0});
    });
    EXPECT_EQ(visited, 1u);
    EXPECT_EQ(CountPositions(*world), 2u);
}
