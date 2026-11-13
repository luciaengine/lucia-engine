// The scenes a build ships (LoadEffectiveBuildScenes): the global list, else the platform's
// own list, else the project's startup scene, so a project fresh from a template builds with
// no setup.
//
// Its own process: the store reads the settings of EngineCore's workspace root, so each test
// points that process-wide root at a temporary project.

#include "Core/Engine.h"
#include "Editor/Settings/BuildSettingsStore.h"
#include "LogCapture.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

class BuildSceneList : public ::testing::Test
{
  protected:
    void WriteProjectSettings(const std::string& json)
    {
        const fs::path settingsPath = m_ProjectRoot / ".Editor" / "ProjectSettings.json";
        fs::create_directories(settingsPath.parent_path());
        std::ofstream(settingsPath, std::ios::binary) << json;
        EngineCore::GetInstance().SetWorkspaceRoot(m_ProjectRoot);
    }

    void WriteScene(const std::string& assetRelative)
    {
        const fs::path scenePath = m_ProjectRoot / "Assets" / assetRelative;
        fs::create_directories(scenePath.parent_path());
        std::ofstream(scenePath, std::ios::binary) << "[scene name=\"Main\"]\n";
    }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_build_scene_list")};
    const fs::path m_ProjectRoot = m_Root.Path() / "project";
};

TEST_F(BuildSceneList, ProjectWithNoSceneListShipsItsStartupScene)
{
    WriteScene("Scenes/Main.scene");
    WriteProjectSettings(R"({ "project.startupScene": "Scenes/Main.scene" })");

    EXPECT_EQ(Editor::LoadEffectiveBuildScenes("Windows"), std::vector<std::string>{"Scenes/Main.scene"});
}

TEST_F(BuildSceneList, MissingStartupSceneShipsNone)
{
    WriteProjectSettings(R"({ "project.startupScene": "Scenes/Deleted.scene" })");

    EXPECT_TRUE(Editor::LoadEffectiveBuildScenes("Windows").empty());
}

TEST_F(BuildSceneList, ANestedStartupSceneShipsNoneAndWarnsOnceWithTheKeyToUse)
{
    WriteScene("Scenes/Main.scene");
    WriteProjectSettings(R"({ "project": { "startupScene": "Scenes/Main.scene" } })");

    std::vector<std::string> lines;
    {
        TestLog::ScopedLogCapture capture(&lines, Logger::LogLevel::Warning);
        EXPECT_TRUE(Editor::LoadEffectiveBuildScenes("Windows").empty());
    }
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find(R"(use the top-level key "project.startupScene")"), std::string::npos) << lines[0];
}

TEST_F(BuildSceneList, GlobalListWinsOverTheStartupScene)
{
    WriteProjectSettings(R"({ "project.startupScene": "Scenes/Main.scene",
                              "build.globalScenes": "Scenes/Menu.scene\nScenes/Level.scene" })");

    EXPECT_EQ(Editor::LoadEffectiveBuildScenes("Windows"),
              (std::vector<std::string>{"Scenes/Menu.scene", "Scenes/Level.scene"}));
}

TEST_F(BuildSceneList, ListLinesAreTrimmedAndBlankLinesSkipped)
{
    WriteProjectSettings(R"({ "build.globalScenes": "  Scenes/My Level.scene\r\n\n\tScenes/Zürich.scene \n" })");

    EXPECT_EQ(Editor::LoadEffectiveBuildScenes("Windows"),
              (std::vector<std::string>{"Scenes/My Level.scene", "Scenes/Z\xC3\xBCrich.scene"}));
}

TEST_F(BuildSceneList, PlatformOffTheGlobalListShipsItsOwnList)
{
    WriteProjectSettings(R"({ "project.startupScene": "Scenes/Main.scene",
                              "build.globalScenes": "Scenes/Menu.scene",
                              "build.platform.Windows.useGlobalScenes": false,
                              "build.platform.Windows.scenes": "Scenes/Desktop.scene" })");

    EXPECT_EQ(Editor::LoadEffectiveBuildScenes("Windows"), std::vector<std::string>{"Scenes/Desktop.scene"});
}

TEST_F(BuildSceneList, ProjectThatNamesNoSceneShipsNone)
{
    WriteProjectSettings(R"({ "schemaVersion": 1 })");

    EXPECT_TRUE(Editor::LoadEffectiveBuildScenes("Windows").empty());
}

} // namespace
