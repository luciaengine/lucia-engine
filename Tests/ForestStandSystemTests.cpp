#include "Engine/Rendering/Forest/ForestStandSystem.h"

#include "ForestPlacementFeature.h"
#include "ForestPlacementSession.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/ForestStand.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "EngineLogCapture.h"
#include "PackedPlacementTestArchive.h"
#include "Rendering/Core/Device.h"
#include "Scene/SceneIO.h"
#include "Tests/ScopedEnvVar.h"

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

// An overview mesh of `quads` quads.
Mesh MakeOverviewMesh(uint32 quads)
{
    Mesh mesh{};
    mesh.Vertices.resize(4);
    for (uint32 quad = 0; quad < quads; ++quad)
        mesh.Indices.insert(mesh.Indices.end(), {0, 1, 2, 0, 2, 3});
    return mesh;
}

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

TEST_F(ForestStandSystemTest, InspectorStatusFollowsTheLastDetailViewPlan)
{
    m_Services.Spine().Initialize(m_Services);
    ECS::World world;
    const auto entity = CreateStand(world);
    auto* stand = world.GetComponentForWrite<Components::ForestStand>(entity);
    stand->TreeModel = Components::ModelRef(m_Archive);
    stand->DetailDistance = 100.0f;
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    const auto* feature = m_Services.GetFeature<ForestPlacementFeature>();
    ASSERT_NE(feature, nullptr);
    const auto sessions = ForestPlacementFeatureTestAccess::Sessions(*feature);
    ASSERT_EQ(sessions.size(), 1u);
    auto& session = *sessions.front();
    session.DetailPlanEpoch = m_Services.Spine().WorldFrameEpoch();
    session.Plan.DetailBudget = {.Eligible = 7, .Selected = 4, .Resident = 3, .NearestOmitted = 53.0};
    m_Services.Spine().BeginAppFrame();
    system.Update(world, 0.0f);
    const auto* status = system.FindStatus(world.GetWorldId(), entity);
    ASSERT_NE(status, nullptr);
    ASSERT_TRUE(status->DetailBudget);
    EXPECT_EQ(status->DetailBudget->Eligible, 7u);
    EXPECT_EQ(status->DetailBudget->Selected, 4u);
    EXPECT_EQ(status->DetailBudget->Resident, 3u);
    EXPECT_EQ(status->DetailBudget->NearestOmitted, 53.0);

    EXPECT_EQ(status->DetailView, Rendering::ViewPurpose::EditorScene);

    // The Game view takes detail ownership with enough slots for its visible pages.
    session.DetailPlanEpoch = m_Services.Spine().WorldFrameEpoch();
    session.DetailPlanView = Rendering::ViewPurpose::Game;
    session.Plan.DetailBudget = {.Eligible = 1, .Selected = 1, .Resident = 1};
    system.Update(world, 0.0f);
    EXPECT_FALSE(status->DetailBudget);

    session.Plan.DetailBudget = {.Eligible = 8, .Selected = 4, .Resident = 4, .NearestOmitted = 72.0};
    system.Update(world, 0.0f);
    ASSERT_TRUE(status->DetailBudget);
    EXPECT_EQ(status->DetailView, Rendering::ViewPurpose::Game);
    EXPECT_EQ(status->DetailBudget->NearestOmitted, 72.0);

    // No detail view planned in the intervening frame.
    m_Services.Spine().BeginAppFrame();
    m_Services.Spine().BeginAppFrame();
    system.Update(world, 0.0f);
    EXPECT_FALSE(system.FindStatus(world.GetWorldId(), entity)->DetailBudget);
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

TEST_F(ForestStandSystemTest, DetailSettingsApplyToTheAttachedSession)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    auto stand = *world.GetComponent<Components::ForestStand>(entity);
    stand.TreeModel = Components::ModelRef(m_Archive);
    stand.DetailDistance = 275.0f;
    stand.NearMeshBudget = 96;
    world.AddComponentImmediate(entity, stand);
    ForestStandSystem system(&m_Services, &m_Assets);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        system.Update(world, 0.0f);
        const auto* feature = m_Services.GetFeature<ForestPlacementFeature>();
        ASSERT_NE(feature, nullptr);
        const auto session = ForestPlacementFeatureTestAccess::Sessions(*feature).front();

        auto* written = world.GetComponentForWrite<Components::ForestStand>(entity);
        written->DetailDistance = 120.0f;
        written->NearMeshBudget = 12;
        system.Update(world, 0.0f);
        const auto sessions = ForestPlacementFeatureTestAccess::Sessions(*feature);
        ASSERT_EQ(sessions.size(), 1u);
        EXPECT_EQ(sessions.front(), session);
        EXPECT_EQ(session->DetailDistance, 120.0f);
        EXPECT_EQ(session->NearBudget, 12u);
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "attached with"), 1u);
}

TEST_F(ForestStandSystemTest, ShadowCastingOffTurnsTheSessionCastingOff)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    auto stand = *world.GetComponent<Components::ForestStand>(entity);
    stand.TreeModel = Components::ModelRef(m_Archive);
    stand.DetailDistance = 275.0f;
    world.AddComponentImmediate(entity, stand);
    world.GetComponentForWrite<Components::MeshRenderer>(entity)->shadowCasting = Components::ShadowCasting::Off;
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    const auto* feature = m_Services.GetFeature<ForestPlacementFeature>();
    ASSERT_NE(feature, nullptr);
    const auto session = ForestPlacementFeatureTestAccess::Sessions(*feature).front();
    EXPECT_FALSE(session->CastsShadows);
    EXPECT_TRUE(session->DrawsColor);
}

TEST_F(ForestStandSystemTest, ToggleKeepsTheAttachedStand)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    auto stand = *world.GetComponent<Components::ForestStand>(entity);
    stand.TreeModel = Components::ModelRef(m_Archive);
    stand.DetailDistance = 275.0f;
    world.AddComponentImmediate(entity, stand);
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    const auto* feature = m_Services.GetFeature<ForestPlacementFeature>();
    ASSERT_NE(feature, nullptr);
    const auto session = ForestPlacementFeatureTestAccess::Sessions(*feature).front();
    EXPECT_TRUE(session->CastsShadows);

    world.GetComponentForWrite<Components::MeshRenderer>(entity)->shadowCasting = Components::ShadowCasting::Off;
    system.Update(world, 0.0f);
    EXPECT_FALSE(session->CastsShadows);
    world.GetComponentForWrite<Components::MeshRenderer>(entity)->shadowCasting = Components::ShadowCasting::On;
    system.Update(world, 0.0f);
    EXPECT_TRUE(session->CastsShadows);
    const auto sessions = ForestPlacementFeatureTestAccess::Sessions(*feature);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front(), session);
}

TEST_F(ForestStandSystemTest, ShadowsOnlyCastsAndDrawsNoColor)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    auto stand = *world.GetComponent<Components::ForestStand>(entity);
    stand.TreeModel = Components::ModelRef(m_Archive);
    stand.DetailDistance = 275.0f;
    world.AddComponentImmediate(entity, stand);
    world.GetComponentForWrite<Components::MeshRenderer>(entity)->shadowCasting = Components::ShadowCasting::ShadowsOnly;
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    const auto* feature = m_Services.GetFeature<ForestPlacementFeature>();
    ASSERT_NE(feature, nullptr);
    const auto session = ForestPlacementFeatureTestAccess::Sessions(*feature).front();
    EXPECT_TRUE(session->CastsShadows);
    EXPECT_FALSE(session->DrawsColor);
}

TEST_F(ForestStandSystemTest, ANearMeshBudgetFromOrToZeroAttachesTheStandAgain)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    auto stand = *world.GetComponent<Components::ForestStand>(entity);
    stand.TreeModel = Components::ModelRef(m_Archive);
    stand.DetailDistance = 275.0f;
    world.AddComponentImmediate(entity, stand);
    ForestStandSystem system(&m_Services, &m_Assets);
    system.Update(world, 0.0f);
    const auto* feature = m_Services.GetFeature<ForestPlacementFeature>();
    ASSERT_NE(feature, nullptr);
    // The test holds each replaced session, so the newest session is the last one.
    const auto cardsOnly = ForestPlacementFeatureTestAccess::Sessions(*feature).back();

    world.GetComponentForWrite<Components::ForestStand>(entity)->NearMeshBudget = 12;
    system.Update(world, 0.0f);
    const auto withMeshes = ForestPlacementFeatureTestAccess::Sessions(*feature).back();
    EXPECT_NE(withMeshes, cardsOnly);
    EXPECT_EQ(withMeshes->NearBudget, 12u);

    world.GetComponentForWrite<Components::ForestStand>(entity)->NearMeshBudget = 24;
    system.Update(world, 0.0f);
    EXPECT_EQ(ForestPlacementFeatureTestAccess::Sessions(*feature).back(), withMeshes);
    EXPECT_EQ(withMeshes->NearBudget, 24u);

    world.GetComponentForWrite<Components::ForestStand>(entity)->NearMeshBudget = 0;
    system.Update(world, 0.0f);
    const auto backToCards = ForestPlacementFeatureTestAccess::Sessions(*feature).back();
    EXPECT_NE(backToCards, withMeshes);
}

TEST_F(ForestStandSystemTest, AChangedArchiveFileAttachesTheStandAgain)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    ForestStandSystem system(&m_Services, &m_Assets);
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        system.Update(world, 0.0f);
        m_Assets.GetEventDispatcher().DispatchEvent(AssetEvent(AssetEventType::AssetModified, GUID::Generate(),
                                                               AssetType::ForestPlacementArchive, "Other.geforest"));
        system.Update(world, 0.0f);
        m_Assets.GetEventDispatcher().DispatchEvent(AssetEvent(AssetEventType::AssetModified, m_Archive,
                                                               AssetType::ForestPlacementArchive, "Stand.geforest"));
        system.Update(world, 0.0f);
        system.Update(world, 0.0f);
        EXPECT_TRUE(IsAttached(system, world, entity));
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "attached with"), 2u);
}

// A stand system whose render services own a headless device, so registered meshes become drawable.
// Device-gated: each test skips without a Vulkan device.
class ForestStandSystemDeviceTest : public ForestStandSystemTest
{
protected:
    void SetUp() override
    {
        ForestStandSystemTest::SetUp();
        Rendering::DeviceDesc desc{};
        desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        m_Device = Rendering::DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "No Vulkan device";
        ASSERT_TRUE(m_Services.Initialize(m_Device.get()));
    }

    void TearDown() override
    {
        if (m_Device)
        {
            m_Device->WaitForIdle();
            m_Services.Shutdown();
            m_Device->Shutdown();
        }
        ForestStandSystemTest::TearDown();
    }

    Rendering::Tests::ScopedEnvVar m_Headless{"GE_HEADLESS_TEST", "1"};
    std::unique_ptr<Rendering::IDevice> m_Device;
};

TEST_F(ForestStandSystemDeviceTest, AnOverviewThatDoesNotMatchItsArchiveWarnsUntilItMatches)
{
    ECS::World world;
    const auto entity = CreateStand(world);
    ForestStandSystem system(&m_Services, &m_Assets);
    auto& registry = m_Services.GetMeshGPURegistry();
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        // The overview is not resident yet: no verdict.
        system.Update(world, 0.0f);
        EXPECT_TRUE(IsAttached(system, world, entity));

        registry.RegisterSubmesh({m_OverviewModel, 0}, MakeOverviewMesh(3));
        system.Update(world, 0.0f);
        system.Update(world, 0.0f);
        EXPECT_FALSE(IsAttached(system, world, entity));
        EXPECT_NE(system.FindStatus(world.GetWorldId(), entity)->Problem.find("Cook the overview mesh again"),
                  std::string::npos);

        registry.UnregisterSubmesh({m_OverviewModel, 0});
        registry.RegisterSubmesh({m_OverviewModel, 0}, MakeOverviewMesh(2));
        system.Update(world, 0.0f);
        EXPECT_TRUE(IsAttached(system, world, entity));
        EXPECT_TRUE(system.FindStatus(world.GetWorldId(), entity)->Problem.empty());
        Logger::Log::Flush();
    }
    EXPECT_EQ(CountLines(lines, "Cook the overview mesh again"), 1u);
}

} // namespace
} // namespace GameEngine
