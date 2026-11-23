#include "Engine/Rendering/Forest/ForestStandSystem.h"

#include "ForestPlacementFeature.h"
#include "ForestPlacementSession.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/ForestStand.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "EngineLogCapture.h"
#include "PackedPlacementTestArchive.h"
#include "Scene/SceneIO.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{
struct ForestPlacementFeatureTestAccess
{
    static std::vector<std::shared_ptr<ForestPlacementSession>> Sessions(const ForestPlacementFeature& feature)
    {
        std::vector<std::shared_ptr<ForestPlacementSession>> sessions;
        for (const auto& entry : feature.m_Sessions)
            if (auto session = entry.lock())
                sessions.push_back(std::move(session));
        return sessions;
    }
};
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace
{
using Engine::Renderer::ForestPlacementFeature;
using Engine::Renderer::ForestPlacementFeatureTestAccess;
using Engine::Renderer::ForestStandSystem;

size_t CountLines(const std::vector<std::string>& lines, std::string_view text)
{
    return static_cast<size_t>(
        std::ranges::count_if(lines, [text](const std::string& line) { return line.find(text) != std::string::npos; }));
}

class ForestStandSystemTest : public PageStreaming::PackedPlacementArchiveTest
{
protected:
    static bool IsAttached(const ForestStandSystem& system, const ECS::World& world, ECS::EntityHandle entity)
    {
        const auto* status = system.FindStatus(world.GetWorldId(), entity);
        return status && status->Attached;
    }

    // The one stand entity of the world.
    static ECS::EntityHandle FindStand(ECS::World& world)
    {
        ECS::EntityHandle found{};
        world.Query<ECS::Read<Components::ForestStand>>().Each(
            [&found](ECS::EntityHandle entity, const Components::ForestStand&) { found = entity; });
        return found;
    }

    void SetUp() override
    {
        std::filesystem::create_directories(m_Scratch.Path() / "Assets");
        WriteStore(m_Scratch.Path() / "Assets" / "Stand.geforest");
        ASSERT_TRUE(m_Assets.Initialize(m_Scratch.Path() / "Assets", nullptr, m_Scratch.Path() / "registry.assetdb",
                                        m_Scratch.Path() / "cache"));
        m_Assets.WaitForStartupScan();
        m_Archive = m_Assets.GetRegistry().GetAssetGUID(m_Scratch.Path() / "Assets" / "Stand.geforest");
        ASSERT_FALSE(m_Archive.IsNull());
    }

    void TearDown() override { m_Assets.Shutdown(); }

    ECS::EntityHandle CreateStand(ECS::World& world, Rendering::RenderLayerMask layers = Rendering::RenderLayerMask::None())
    {
        Components::MeshRenderer renderer{};
        renderer.modelAssetGuid = Components::ModelRef(m_OverviewModel);
        renderer.RenderLayers = layers;
        Components::ForestStand stand{};
        stand.Archive = Components::ForestPlacementArchiveRef(m_Archive);
        const auto entity = world.CreateEntity();
        world.AddComponentImmediate(entity, renderer);
        world.AddComponentImmediate(entity, stand);
        return entity;
    }

    AssetManager m_Assets;
    GUID m_Archive;
    const GUID m_OverviewModel = GUID::Generate();
    Engine::Renderer::RenderServices m_Services;
};

TEST_F(ForestStandSystemTest, SceneFileKeepsEveryForestStandField)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    Components::ForestStand authored{};
    authored.Archive = Components::ForestPlacementArchiveRef(m_Archive);
    authored.TreeModel = Components::ModelRef(GUID::Generate());
    authored.DetailDistance = 275.0f;
    authored.NearMeshBudget = 96;
    world.AddComponentImmediate(entity, authored);

    const auto scene = m_Scratch.Path() / "Stand.scene";
    ASSERT_TRUE(Scene::WriteSceneFile(world, scene));
    ECS::World loaded;
    ASSERT_TRUE(Scene::ReadSceneInto(loaded, scene));
    std::vector<Components::ForestStand> stands;
    loaded.Query<ECS::Read<Components::ForestStand>>().Each(
        [&stands](const Components::ForestStand& stand) { stands.push_back(stand); });
    ASSERT_EQ(stands.size(), 1u);
    EXPECT_EQ(stands.front().Archive, authored.Archive);
    EXPECT_EQ(stands.front().TreeModel, authored.TreeModel);
    EXPECT_FLOAT_EQ(stands.front().DetailDistance, 275.0f);
    EXPECT_EQ(stands.front().NearMeshBudget, 96u);
}

TEST_F(ForestStandSystemTest, EachSceneLoadAttachesOneStandAndReleasesTheLast)
{
    ECS::World world;
    CreateStand(world);
    const auto scene = m_Scratch.Path() / "Stand.scene";
    ASSERT_TRUE(Scene::WriteSceneFile(world, scene));

    ForestStandSystem system(&m_Services, &m_Assets);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        for (int load = 0; load < 2; ++load)
        {
            ASSERT_TRUE(Scene::ReadSceneInto(world, scene));
            system.Update(world, 0.0f);
            system.Update(world, 0.0f);
            EXPECT_TRUE(IsAttached(system, world, FindStand(world))) << "load " << load;
        }
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "attached with 2 pages and 3 placements"), 2u);
}

TEST_F(ForestStandSystemTest, RemovingTheComponentReleasesTheStand)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    ASSERT_TRUE(IsAttached(system, world, entity));

    world.RemoveComponentImmediate<Components::ForestStand>(entity);
    system.Update(world, 0.0f);
    EXPECT_EQ(system.FindStatus(world.GetWorldId(), entity), nullptr);
}

TEST_F(ForestStandSystemTest, AnOverviewThatDrawsItselfWarnsOnceAndAttachesWhenFixed)
{
    ECS::World world;
    const auto entity = CreateStand(world, Rendering::RenderLayerMask::Default());
    ForestStandSystem system(&m_Services, &m_Assets);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        system.Update(world, 0.0f);
        system.Update(world, 0.0f);
        EXPECT_FALSE(IsAttached(system, world, entity));
        EXPECT_NE(system.FindStatus(world.GetWorldId(), entity)->Problem.find("Set its render layers to None"),
                  std::string::npos);

        world.GetComponentForWrite<Components::MeshRenderer>(entity)->RenderLayers = Rendering::RenderLayerMask::None();
        system.Update(world, 0.0f);
        EXPECT_TRUE(IsAttached(system, world, entity));
        EXPECT_TRUE(system.FindStatus(world.GetWorldId(), entity)->Problem.empty());
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "Set its render layers to None"), 1u);
}

TEST_F(ForestStandSystemTest, UnrelatedSpawnsKeepTheStand)
{
    ECS::World world;
    CreateStand(world);
    ForestStandSystem system(&m_Services, &m_Assets);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        for (int frame = 0; frame < 5; ++frame)
        {
            world.AddComponentImmediate(world.CreateEntity(), Components::MeshRenderer{});
            system.Update(world, 0.0f);
        }
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "attached with"), 1u);
}

TEST_F(ForestStandSystemTest, ANanDetailDistanceWarnsOnceAndDrawsTheOverview)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    auto stand = *world.GetComponent<Components::ForestStand>(entity);
    stand.TreeModel = Components::ModelRef(m_Archive);
    stand.DetailDistance = std::numeric_limits<float>::quiet_NaN();
    world.AddComponentImmediate(entity, stand);
    ForestStandSystem system(&m_Services, &m_Assets);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        for (int frame = 0; frame < 5; ++frame)
        {
            (void)world.GetComponentForWrite<Components::MeshRenderer>(entity);
            system.Update(world, 0.0f);
        }
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "Detail Distance must be a number of meters"), 1u);
    EXPECT_EQ(CountLines(lines, "attached with"), 1u);
    EXPECT_TRUE(IsAttached(system, world, entity));
}

TEST_F(ForestStandSystemTest, ATreeModelAttachesADetailSessionWithTheBudgetCapped)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    auto stand = *world.GetComponent<Components::ForestStand>(entity);
    stand.TreeModel = Components::ModelRef(m_Archive);
    stand.DetailDistance = 275.0f;
    stand.NearMeshBudget = 100'000;
    world.AddComponentImmediate(entity, stand);
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    ASSERT_TRUE(IsAttached(system, world, entity));
    const auto* feature = m_Services.GetFeature<ForestPlacementFeature>();
    ASSERT_NE(feature, nullptr);
    {
        const auto sessions = ForestPlacementFeatureTestAccess::Sessions(*feature);
        ASSERT_EQ(sessions.size(), 1u);
        EXPECT_EQ(sessions.front()->DetailDistance, 275.0f);
        EXPECT_GT(sessions.front()->NearBudget, 0u);
        EXPECT_LE(sessions.front()->NearBudget, Components::ForestStand::kMaxNearMeshBudget);
    }

    // A Detail Distance of 0 draws the overview only, with no warning.
    world.GetComponentForWrite<Components::ForestStand>(entity)->DetailDistance = 0.0f;
    system.Update(world, 0.0f);
    EXPECT_TRUE(IsAttached(system, world, entity));
    EXPECT_TRUE(system.FindStatus(world.GetWorldId(), entity)->Problem.empty());
    EXPECT_TRUE(ForestPlacementFeatureTestAccess::Sessions(*feature).empty());
}

TEST_F(ForestStandSystemTest, ASnapshotRestoreBringsBackARemovedStand)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    const auto snapshot = world.SerializeWorld();
    world.RemoveComponentImmediate<Components::ForestStand>(entity);
    system.Update(world, 0.0f);
    ASSERT_EQ(system.FindStatus(world.GetWorldId(), entity), nullptr);

    world.DeserializeWorld(snapshot);
    system.Update(world, 0.0f);
    EXPECT_TRUE(IsAttached(system, world, entity));
    world.GetComponentForWrite<Components::ForestStand>(entity)->TreeModel = Components::ModelRef(GUID::Generate());
    system.Update(world, 0.0f);
    EXPECT_NE(system.FindStatus(world.GetWorldId(), entity)->Problem.find("tree model is not in the project"),
              std::string::npos);
}

} // namespace
} // namespace GameEngine
