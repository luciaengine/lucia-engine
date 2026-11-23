#include "Engine/Rendering/Forest/ForestStandSystem.h"

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
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{
struct ForestStandSystemTestAccess
{
    static size_t LiveRegistrations(const ForestStandSystem& system)
    {
        return static_cast<size_t>(std::ranges::count_if(
            system.m_Stands, [](const auto& entry) { return static_cast<bool>(entry.second.Registration); }));
    }
};
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace
{
using Engine::Renderer::ForestStandSystem;
using Engine::Renderer::ForestStandSystemTestAccess;

size_t CountLines(const std::vector<std::string>& lines, std::string_view text)
{
    return static_cast<size_t>(
        std::ranges::count_if(lines, [text](const std::string& line) { return line.find(text) != std::string::npos; }));
}

class ForestStandSystemTest : public PageStreaming::PackedPlacementArchiveTest
{
protected:
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
            EXPECT_EQ(ForestStandSystemTestAccess::LiveRegistrations(system), 1u) << "load " << load;
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
    ASSERT_EQ(ForestStandSystemTestAccess::LiveRegistrations(system), 1u);

    world.RemoveComponentImmediate<Components::ForestStand>(entity);
    system.Update(world, 0.0f);
    EXPECT_EQ(ForestStandSystemTestAccess::LiveRegistrations(system), 0u);
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
        EXPECT_EQ(ForestStandSystemTestAccess::LiveRegistrations(system), 0u);

        world.GetComponentForWrite<Components::MeshRenderer>(entity)->RenderLayers = Rendering::RenderLayerMask::None();
        system.Update(world, 0.0f);
        EXPECT_EQ(ForestStandSystemTestAccess::LiveRegistrations(system), 1u);
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "Set its render layers to None"), 1u);
}

} // namespace
} // namespace GameEngine
