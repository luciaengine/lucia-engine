#include "Core/Application.h"
#include "Core/Engine.h"
#include "SceneLoadTracker/SceneLoadTracker.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace GameEngine
{
namespace
{
class EngineSceneReadinessTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OriginalDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("EngineSceneReadiness");
        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        auto& engine = EngineCore::GetInstance();
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(engine.Initialize(config));
    }

    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code error;
        std::filesystem::current_path(m_OriginalDirectory, error);
        std::filesystem::remove_all(m_Workspace, error);
    }

    std::filesystem::path m_OriginalDirectory;
    std::filesystem::path m_Workspace;
};

TEST_F(EngineSceneReadinessTests, FrameUpdatesPublishGameWorkAndRaiseReadyAfterCompletion)
{
    auto& engine = EngineCore::GetInstance();
    auto& tracker = engine.GetSceneLoadTracker();
    auto category = tracker.RegisterCategory({"game.spawn", "Spawning the scene"});
    auto ticket = category.BeginWork(tracker.Intern("Spawn actors"), 1.0);
    int readyEvents = 0;
    auto subscription = tracker.Ready.Subscribe([&readyEvents] { ++readyEvents; });
    constexpr double kFrameSeconds = 1.0 / 60.0;
    engine.Update(kFrameSeconds);
    EXPECT_FALSE(tracker.IsReadyToShow());
    ticket.Start();
    ticket.Complete();
    engine.Update(kFrameSeconds);
    EXPECT_TRUE(tracker.IsReadyToShow());
    EXPECT_EQ(readyEvents, 1);
    engine.Update(kFrameSeconds);
    EXPECT_EQ(readyEvents, 1);
}

TEST_F(EngineSceneReadinessTests, RenderingCategoriesRemainRegisteredWhenNoRendererIsBound)
{
    auto& engine = EngineCore::GetInstance();
    constexpr double kFrameSeconds = 1.0 / 60.0;
    engine.Update(kFrameSeconds);
    const auto& tracker = engine.GetSceneLoadTracker();
    for (const std::string_view id : {"scene.resolve", "shader.base-compile", "shader.variant",
                                      "gpu.pipeline", "render.texture-upload"})
    {
        const auto& categories = tracker.GetStatus().Categories;
        const auto found = std::find_if(categories.begin(), categories.end(),
                                       [id](const auto& row) { return row.Id == id; });
        ASSERT_NE(found, categories.end()) << id;
        EXPECT_FALSE(found->HoldsReady()) << id;
    }
    EXPECT_TRUE(tracker.IsReadyToShow());
}
} // namespace
} // namespace GameEngine
