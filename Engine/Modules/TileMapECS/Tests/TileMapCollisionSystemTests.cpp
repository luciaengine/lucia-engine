#include "Components/TileMap/TileMapCollider.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/World.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/RegisterTileMap.h"
#include "TileMapECS/Systems/TileMapCollisionSystem.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "TileMapECS/TileSurfaceQuery.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace GameEngine;
using namespace GameEngine::TileMapECS;
using GameEngine::TileMap::MakeCell;

namespace
{

constexpr const char* kMapGuidText = "aaaaaaaa-1111-2222-3333-444444444444";
constexpr const char* kTileSetGuidText = "bbbbbbbb-1111-2222-3333-444444444444";
constexpr uint32 kGroundLayerId = 1;
constexpr uint32 kSquareTileId = 1;
constexpr uint32 kOneWayTileId = 2;
constexpr uint32 kNotchTileId = 3;
constexpr float32 kDt = 1.0f / 60.0f;

Physics::PhysicsWorldSettings SmallDeterministicSettings()
{
    Physics::PhysicsWorldSettings settings{};
    settings.gravity = Physics::Vector3(0.0f, -9.81f, 0.0f);
    settings.numThreads = 1;
    settings.maxBodies = 1024;
    settings.maxBodyPairs = 1024;
    settings.maxContactConstraints = 1024;
    settings.tempAllocatorBytes = 16u * 1024u * 1024u;
    return settings;
}

std::filesystem::path FixturePath(const std::string& suffix)
{
    const std::string test =
        ::testing::UnitTest::GetInstance()->current_test_info()->name();
    return std::filesystem::temp_directory_path() /
           ("TileMapCollisionSystemTests_" + test + suffix);
}

void WriteBytes(const std::filesystem::path& path, const Vector<uint8>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/**
 * Store + physics world + one tilemap entity, built through the real
 * serializers so the fixture cannot drift from the asset formats. Tile 1 is a
 * full square; the map starts with a 4-cell floor row in chunk (0,0) and one
 * cell in chunk (1,0).
 */
class TileMapCollisionSystemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_FALSE(TileMapRuntimeStore::IsInitialized());
        TileMapRuntimeStore::Initialize();
        if (PhysicsECS::PhysicsWorldService::IsInitialized())
            PhysicsECS::PhysicsWorldService::Shutdown();
        PhysicsECS::PhysicsWorldService::Initialize(SmallDeterministicSettings());

        m_MapGuid = GUID(kMapGuidText);
        m_TileSetGuid = GUID(kTileSetGuidText);
        m_MapPath = FixturePath(".tilemap");
        m_TileSetPath = FixturePath(".tileset");

        {
            TileMap::TileSetAsset tileSet(m_TileSetGuid, m_TileSetPath);
            TileMap::TileSetTile square;
            square.Id = kSquareTileId;
            square.CollisionPolygons.push_back(TileMap::TileCollisionPolygon{
                {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}});
            tileSet.Tiles().push_back(square);

            // Same full-square geometry, authored one-way: the merge refuses to
            // fuse it with the solid run, and the baker must give it its own
            // body so its contact flags mean something.
            TileMap::TileSetTile oneWay = square;
            oneWay.Id = kOneWayTileId;
            oneWay.CollisionMode = TileMap::TileCollisionMode::OneWay;
            tileSet.Tiles().push_back(oneWay);

            // Concave: a cup whose notch a convex hull would fill in, so the
            // difference between the two is where a ball comes to rest.
            TileMap::TileSetTile notch;
            notch.Id = kNotchTileId;
            notch.CollisionPolygons.push_back(TileMap::TileCollisionPolygon{{{0.0f, 0.0f},
                                                                             {1.0f, 0.0f},
                                                                             {1.0f, 1.0f},
                                                                             {0.8f, 1.0f},
                                                                             {0.8f, 0.25f},
                                                                             {0.2f, 0.25f},
                                                                             {0.2f, 1.0f},
                                                                             {0.0f, 1.0f}}});
            tileSet.Tiles().push_back(notch);

            Vector<uint8> bytes;
            ASSERT_TRUE(tileSet.SaveToData(bytes));
            WriteBytes(m_TileSetPath, bytes);
        }

        {
            TileMap::TileMapAsset map(m_MapGuid, m_MapPath);
            TileMap::TileMapData& data = map.Data();
            data.CellWidth = 1.0f;
            data.CellHeight = 1.0f;
            const uint32 layerId = data.AddLayer("Ground", TileMap::TileMapLayerType::Tiles);
            ASSERT_EQ(layerId, kGroundLayerId);
            TileMap::TileMapLayerData& ground = *data.FindLayer(layerId);
            ground.TileSetGuid = m_TileSetGuid;
            for (int32 x = 0; x < 4; ++x)
                ground.SetCell(x, 0, MakeCell(kSquareTileId));
            ground.SetCell(40, 0, MakeCell(kSquareTileId)); // chunk (1,0)
            Vector<uint8> bytes;
            ASSERT_TRUE(map.SaveToData(bytes));
            WriteBytes(m_MapPath, bytes);
        }

        ASSERT_TRUE(Store().LoadMapFromFile(m_MapGuid, m_MapPath));
        ASSERT_TRUE(Store().LoadTileSetFromFile(m_TileSetGuid, m_TileSetPath));

        m_Entity = m_World.CreateEntity();
        m_World.AddComponentImmediate<Components::Transform>(m_Entity, Components::Transform{});
        m_World.AddComponentImmediate<Components::WorldTransform>(m_Entity,
                                                                  Components::WorldTransform{});
        Components::TileMapLayerRef layerRef{};
        layerRef.TileMap.Set(m_MapGuid);
        layerRef.LayerId = kGroundLayerId;
        m_World.AddComponentImmediate<Components::TileMapLayerRef>(m_Entity, layerRef);
        m_World.AddComponentImmediate<Components::TileMapCollider>(m_Entity,
                                                                   Components::TileMapCollider{});
    }

    void TearDown() override
    {
        PhysicsECS::PhysicsWorldService::Shutdown();
        TileMapRuntimeStore::Shutdown();
        std::error_code ec;
        std::filesystem::remove(m_MapPath, ec);
        std::filesystem::remove(m_TileSetPath, ec);
    }

    TileMapRuntimeStore& Store() { return TileMapRuntimeStore::Get(); }
    Physics::PhysicsWorld& PhysicsWorld() { return PhysicsECS::PhysicsWorldService::Get(); }

    GUID m_MapGuid;
    GUID m_TileSetGuid;
    std::filesystem::path m_MapPath;
    std::filesystem::path m_TileSetPath;
    ECS::World m_World;
    ECS::EntityHandle m_Entity;
    TileMapCollisionSystem m_System;
};

} // namespace

TEST_F(TileMapCollisionSystemTest, InitialBuildCreatesOneBodyPerOccupiedChunk)
{
    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.ChunkBodyCount(), 2u);
    EXPECT_EQ(PhysicsWorld().GetBodyCount(), 2u);
    EXPECT_EQ(m_System.ChunkRebuildCount(), 2u);
}

TEST_F(TileMapCollisionSystemTest, SteadyStateDoesNotRebuild)
{
    m_System.Update(m_World, kDt);
    const uint64 rebuilds = m_System.ChunkRebuildCount();
    m_System.Update(m_World, kDt);
    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.ChunkRebuildCount(), rebuilds);
}

TEST_F(TileMapCollisionSystemTest, CellEditRebuildsOnlyTheTouchedChunk)
{
    m_System.Update(m_World, kDt);
    const uint64 rebuilds = m_System.ChunkRebuildCount();

    // Paint one more cell into chunk (0,0); chunk (1,0) must not rebuild.
    ASSERT_TRUE(Store().SetCell(m_MapGuid, kGroundLayerId, 5, 0, MakeCell(kSquareTileId)));
    m_System.Update(m_World, kDt);

    EXPECT_EQ(m_System.ChunkRebuildCount(), rebuilds + 1);
    EXPECT_EQ(m_System.ChunkBodyCount(), 2u);
}

TEST_F(TileMapCollisionSystemTest, ErasingAChunksLastCellRemovesItsBody)
{
    m_System.Update(m_World, kDt);
    ASSERT_TRUE(Store().SetCell(m_MapGuid, kGroundLayerId, 40, 0, TileMap::kEmptyCell));
    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.ChunkBodyCount(), 1u);
    EXPECT_EQ(PhysicsWorld().GetBodyCount(), 1u);
}

TEST_F(TileMapCollisionSystemTest, DisablingTheColliderTearsDownAllBodies)
{
    m_System.Update(m_World, kDt);
    ASSERT_EQ(m_System.ChunkBodyCount(), 2u);

    m_World.AddComponentImmediate(m_Entity, ECS::ComponentDisabled<Components::TileMapCollider>{});
    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.ChunkBodyCount(), 0u);
    EXPECT_EQ(PhysicsWorld().GetBodyCount(), 0u);

    // Re-enabling rebuilds from scratch.
    m_World.RemoveComponentImmediate<ECS::ComponentDisabled<Components::TileMapCollider>>(m_Entity);
    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.ChunkBodyCount(), 2u);
}

TEST_F(TileMapCollisionSystemTest, EntityDestructionTearsDownAllBodies)
{
    m_System.Update(m_World, kDt);
    ASSERT_EQ(PhysicsWorld().GetBodyCount(), 2u);

    m_World.DestroyEntityImmediate(m_Entity);
    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.ChunkBodyCount(), 0u);
    EXPECT_EQ(PhysicsWorld().GetBodyCount(), 0u);
}

TEST_F(TileMapCollisionSystemTest, PhysicsWorldRestartDropsStaleStateAndRebuilds)
{
    m_System.Update(m_World, kDt);
    ASSERT_EQ(m_System.ChunkBodyCount(), 2u);

    PhysicsECS::PhysicsWorldService::Shutdown();
    PhysicsECS::PhysicsWorldService::Initialize(SmallDeterministicSettings());

    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.ChunkBodyCount(), 2u);
    EXPECT_EQ(PhysicsWorld().GetBodyCount(), 2u);
}

TEST_F(TileMapCollisionSystemTest, SphereComesToRestOnPaintedFloor)
{
    auto& pw = PhysicsWorld();
    m_System.Update(m_World, kDt);
    pw.OptimizeBroadphase();

    Physics::SphereShapeDef sphereDef{};
    sphereDef.radius = 0.5f;
    const Physics::ShapeHandle sphereShape = pw.CreateShape(sphereDef);
    ASSERT_TRUE(sphereShape.IsValid());

    Physics::BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = Physics::MotionType::Dynamic;
    ball.layer = Physics::Layers::Dynamic;
    ball.mass = 1.0f;
    // Above the middle of the 4-cell floor; cells span y in [0, 1].
    ball.position = Physics::Vector3(2.0f, 5.0f, 0.0f);
    ball.startAwake = true;
    const Physics::BodyHandle ballBody = pw.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    for (int i = 0; i < 240; ++i)
        pw.Step(kDt, 1);

    const Physics::Transform t = pw.GetBodyTransform(ballBody);
    // Resting on the floor's top face (y = 1) plus the radius.
    EXPECT_NEAR(t.position.y, 1.5f, 0.05f);
}

TEST_F(TileMapCollisionSystemTest, ASphereSettlesInsideAConcaveTilesNotch)
{
    // The cup's inner floor is at y = 0.25; its convex hull would be the whole
    // cell, and the ball would come to rest on top at y = 1 instead.
    ASSERT_TRUE(Store().SetCell(m_MapGuid, kGroundLayerId, 10, 0, MakeCell(kNotchTileId)));

    auto& pw = PhysicsWorld();
    m_System.Update(m_World, kDt);
    pw.OptimizeBroadphase();

    Physics::SphereShapeDef sphereDef{};
    sphereDef.radius = 0.2f;
    const Physics::ShapeHandle sphereShape = pw.CreateShape(sphereDef);
    ASSERT_TRUE(sphereShape.IsValid());

    Physics::BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = Physics::MotionType::Dynamic;
    ball.layer = Physics::Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = Physics::Vector3(10.5f, 2.0f, 0.0f);
    ball.startAwake = true;
    const Physics::BodyHandle ballBody = pw.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    for (int i = 0; i < 240; ++i)
        pw.Step(kDt, 1);

    EXPECT_NEAR(pw.GetBodyTransform(ballBody).position.y, 0.45f, 0.05f);
}

TEST_F(TileMapCollisionSystemTest, ErasingTheFloorDropsTheRestingSphere)
{
    auto& pw = PhysicsWorld();
    m_System.Update(m_World, kDt);
    pw.OptimizeBroadphase();

    Physics::SphereShapeDef sphereDef{};
    sphereDef.radius = 0.5f;
    const Physics::ShapeHandle sphereShape = pw.CreateShape(sphereDef);
    ASSERT_TRUE(sphereShape.IsValid());

    Physics::BodySettings ball{};
    ball.shape = sphereShape;
    ball.motionType = Physics::MotionType::Dynamic;
    ball.layer = Physics::Layers::Dynamic;
    ball.mass = 1.0f;
    // Sleep stays allowed: the system's wake sweep over a rebuilt chunk is what
    // must drop a sleeper when the ground vanishes beneath it.
    ball.position = Physics::Vector3(2.0f, 2.0f, 0.0f);
    ball.startAwake = true;
    const Physics::BodyHandle ballBody = pw.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    for (int i = 0; i < 240; ++i)
        pw.Step(kDt, 1);
    ASSERT_NEAR(pw.GetBodyTransform(ballBody).position.y, 1.5f, 0.05f);

    // Erase the whole floor row under the sphere, rebuild, keep stepping.
    for (int32 x = 0; x < 4; ++x)
        ASSERT_TRUE(Store().SetCell(m_MapGuid, kGroundLayerId, x, 0, TileMap::kEmptyCell));
    m_System.Update(m_World, kDt);

    for (int i = 0; i < 240; ++i)
        pw.Step(kDt, 1);

    EXPECT_LT(pw.GetBodyTransform(ballBody).position.y, -1.0f);
}

// ---------------------------------------------------------------------------
// Per-tile collision mode and physics material reaching the simulation.
//
// The contact-modify seam snapshots its flags and material index per *body*,
// so a chunk holding more than one (mode, material) group has to bake more than
// one body — otherwise whichever group happened to win would relabel the rest.
// ---------------------------------------------------------------------------

TEST_F(TileMapCollisionSystemTest, AChunkMixingSolidAndOneWayBakesABodyPerGroup)
{
    // Chunk (0,0) already holds a 4-cell solid run; add a one-way run beside it.
    for (int32 x = 8; x < 11; ++x)
        ASSERT_TRUE(Store().SetCell(m_MapGuid, kGroundLayerId, x, 0, MakeCell(kOneWayTileId)));

    m_System.Update(m_World, kDt);

    // Two groups in chunk (0,0), one in chunk (1,0).
    EXPECT_EQ(m_System.OccupiedChunkCount(), 2u);
    EXPECT_EQ(m_System.ChunkBodyCount(), 3u);
    EXPECT_EQ(PhysicsWorld().GetBodyCount(), 3u);
}

TEST_F(TileMapCollisionSystemTest, AUniformChunkStillBakesASingleBody)
{
    // The split is per group, not per tile: a chunk that is all one mode and
    // material must not fan out into a body per rectangle.
    m_System.Update(m_World, kDt);
    EXPECT_EQ(m_System.OccupiedChunkCount(), 2u);
    EXPECT_EQ(m_System.ChunkBodyCount(), 2u);
}

TEST_F(TileMapCollisionSystemTest, ABodyFallsThroughOneWayTilesFromBelowAndLandsFromAbove)
{
    // End-to-end: the tileset says one-way, the baker flags the body, and the
    // engine's default predicate turns that into the platform behaviour.
    for (int32 x = 0; x < 4; ++x)
        ASSERT_TRUE(Store().SetCell(m_MapGuid, kGroundLayerId, x, 4, MakeCell(kOneWayTileId)));
    m_System.Update(m_World, kDt);

    Physics::PhysicsWorld& pw = PhysicsWorld();
    Physics::SphereShapeDef sphere;
    sphere.radius = 0.5f;
    const Physics::ShapeHandle shape = pw.CreateShape(sphere);
    ASSERT_TRUE(shape.IsValid());

    // Launched from below the one-way row, fast enough to cross it.
    Physics::BodySettings ball;
    ball.shape = shape;
    ball.motionType = Physics::MotionType::Dynamic;
    ball.layer = Physics::Layers::Dynamic;
    ball.mass = 1.0f;
    ball.position = Physics::Vector3(2.0f, 2.0f, 0.0f);
    ball.linearVelocity = Physics::Vector3(0.0f, 12.0f, 0.0f);
    ball.startAwake = true;
    const Physics::BodyHandle ballBody = pw.CreateBody(ball);
    ASSERT_TRUE(ballBody.IsValid());

    // Rising: it must get above the platform rather than being stopped by it.
    for (int i = 0; i < 30; ++i)
        pw.Step(kDt, 1);
    EXPECT_GT(pw.GetBodyTransform(ballBody).position.y, 5.5f);

    // Falling back: it must come to rest on top of the row (top at y = 5).
    for (int i = 0; i < 300; ++i)
        pw.Step(kDt, 1);
    EXPECT_NEAR(pw.GetBodyTransform(ballBody).position.y, 5.5f, 0.1f);
}

TEST_F(TileMapCollisionSystemTest, TheSurfaceQueryReportsTheAuthoredModePerCell)
{
    ASSERT_TRUE(Store().SetCell(m_MapGuid, kGroundLayerId, 8, 0, MakeCell(kOneWayTileId)));

    const TileSurface solid = QueryTileSurface(m_MapGuid, kGroundLayerId, 0, 0);
    EXPECT_TRUE(solid.HasTile);
    EXPECT_TRUE(solid.HasCollision);
    EXPECT_FALSE(solid.IsOneWay());

    const TileSurface oneWay = QueryTileSurface(m_MapGuid, kGroundLayerId, 8, 0);
    EXPECT_TRUE(oneWay.HasTile);
    EXPECT_TRUE(oneWay.IsOneWay());

    const TileSurface empty = QueryTileSurface(m_MapGuid, kGroundLayerId, 500, 500);
    EXPECT_FALSE(empty.HasTile);
    EXPECT_FALSE(empty.IsOneWay());
}

// ---------------------------------------------------------------------------
// Schedule placement. Systems inside one wave run in parallel, so anything
// TileMapCollision shares with PhysicsInit — the ContactSurfaceTable index
// space and the backend's body/side tables, neither of which is locked — has
// to be serialized by a declared edge rather than by registration order.
// ---------------------------------------------------------------------------

TEST(TileMapCollisionSchedule, ChunkBakingIsScheduledAfterColliderProvisioning)
{
    ECS::SystemScheduleBuilder builder;
    PhysicsECS::AddPhysicsSystemsToSchedule(builder);
    AddTileMapSystemsToSchedule(builder, /*renderServices*/ nullptr);

    ECS::SystemManager manager;
    // Partial: both module contributions name TransformHierarchy, which the
    // rendering schedule owns and this fixture deliberately omits.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const auto& plan = manager.GetExecutionPlan();
    ASSERT_TRUE(plan.IsValid());

    int initWave = -1;
    int tileWave = -1;
    int stepWave = -1;
    for (std::size_t w = 0; w < plan.Waves.size(); ++w)
    {
        for (std::size_t idx : plan.Waves[w].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(idx);
            if (!name)
                continue;
            const std::string_view n(name);
            if (n == "PhysicsInit")             initWave = static_cast<int>(w);
            else if (n == "TileMapCollision")   tileWave = static_cast<int>(w);
            else if (n == "PhysicsStep")        stepWave = static_cast<int>(w);
        }
    }

    ASSERT_GE(initWave, 0);
    ASSERT_GE(tileWave, 0);
    ASSERT_GE(stepWave, 0);

    EXPECT_LT(initWave, tileWave)
        << "TileMapCollision and PhysicsInit both create bodies and both grow "
           "ContactSurfaceTable; the backend has no body-table lock and the "
           "table documents main-thread-only growth, so sharing a wave is a "
           "data race";
    EXPECT_LT(tileWave, stepWave)
        << "baked chunk bodies must exist before the step integrates against them";
}
