#include "Engine/Build/MobileExportRunner.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/BuildHost.h"
#include "Engine/Build/BuildPlatforms.h"
#include "AssetCore/GUID.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

namespace
{
namespace fs = std::filesystem;
using namespace GameEngine;

class MobileExportRunnerTests : public testing::Test
{
protected:
    fs::path Root = fs::temp_directory_path() / ("mobile test $; " + GUID::Generate().ToString());
    BuildSettings Settings;
    MobileExportInputs Inputs;
    BuildPipeline Pipeline{nullptr};
    std::string Error;

    void SetUp() override
    {
        fs::create_directories(Root / "Assets");
        Settings.projectRoot = Root;
        Settings.outputDirectory = Root / "Build Folder/Android";
        Settings.platformName = "Android";
        Settings.scenes = {"Scenes/Main.scene"};
        Settings.buildConfiguration = "DebugFast";
        Inputs.EngineRoot = Root / "engine";
        Inputs.BuildTool = Inputs.EngineRoot / "Tools/Mobile/build_player.py";
        fs::create_directories(Inputs.BuildTool.parent_path());
        std::ofstream(Inputs.EngineRoot / "CMakeLists.txt") << "# engine fixture";
        fs::create_directories(Root / "content");
        std::ofstream(Root / "content/game.config") << R"({"gameName":"Test game"})";
    }
    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(Root, error);
    }
    void Script(const std::string& body)
    {
        std::ofstream(Inputs.BuildTool) <<
            "import json, pathlib, sys, time\n"
            "assert sys.argv[1] == '--request'\n"
            "r=json.loads(pathlib.Path(sys.argv[2]).read_text())\n"
            "assert r['schemaVersion'] == 1\n"
            "assert r['scenes'] == ['Scenes/Main.scene']\n"
            "assert r['configuration'] == 'DebugFast'\n"
            "assert json.loads(pathlib.Path(r['gameConfigPath']).read_text())\n"
            "out=pathlib.Path(r['outputDirectory'])\n" << body;
    }
    bool Run() { return RunMobileBuildTool(Settings, Pipeline, Inputs, Root / "content", {}, Error); }
    static std::string Receipt(const std::string& id = "r['requestId']", const std::string& artifact = "game.apk")
    {
        return "pathlib.Path(r['resultPath']).write_text(json.dumps({'requestId':" + id
            + ", 'platform':r['platform'], 'configuration':r['configuration'], 'artifact':'" + artifact + "'}))\n";
    }
};

TEST(BuildPlatformsTests, MenuContainsOnlySupportedPublicTargets)
{
    std::vector<std::string> names;
    for (const auto& platform : GetBuildPlatforms())
        names.emplace_back(platform.Name);
    EXPECT_EQ(names, (std::vector<std::string>{"Windows", "Steam", "Linux", "Mac", "Android", "iOS", "Web"}));
    EXPECT_EQ(FindBuildPlatform("Nintendo"), nullptr);
    EXPECT_EQ(FindBuildPlatform("Sony"), nullptr);
}

TEST_F(MobileExportRunnerTests, EngineOwnedToolBuildsWithoutProjectLauncher)
{
    Script("(out/'game.apk').write_bytes(b'package')\n" + Receipt());
    EXPECT_TRUE(Run()) << Error;
}

TEST_F(MobileExportRunnerTests, MissingArtifactAndWrongRequestFail)
{
    Script(Receipt());
    EXPECT_FALSE(Run());
    Script("(out/'game.apk').write_bytes(b'package')\n" + Receipt("'stale'"));
    EXPECT_FALSE(Run());
}

TEST_F(MobileExportRunnerTests, StaleReceiptCannotMakeSuccessfulExitAnExport)
{
    fs::create_directories(Settings.outputDirectory);
    std::ofstream(Settings.outputDirectory / "mobile-build-result.json") << "{}";
    Script("pass\n");
    EXPECT_FALSE(Run());
    EXPECT_FALSE(fs::exists(Settings.outputDirectory / "mobile-build-result.json"));
}

TEST_F(MobileExportRunnerTests, RefusesAuthoredOutputBeforeLaunching)
{
    Script("raise RuntimeError('must not launch')\n");
    Settings.outputDirectory = Root / "Assets/build";
    EXPECT_FALSE(Run());
    EXPECT_NE(Error.find("authored"), std::string::npos);
    Settings.outputDirectory = Root;
    EXPECT_FALSE(Run());
}

TEST_F(MobileExportRunnerTests, RefusesArtifactOutsideOutput)
{
    Script("(out.parent/'game.apk').write_bytes(b'package')\n" + Receipt("r['requestId']", "../game.apk"));
    EXPECT_FALSE(Run());
}

TEST_F(MobileExportRunnerTests, NonzeroExitFailsEvenWithReceipt)
{
    Script("(out/'game.apk').write_bytes(b'package')\n" + Receipt() + "sys.exit(7)\n");
    EXPECT_FALSE(Run());
    EXPECT_NE(Error.find("exit 7"), std::string::npos);
}

TEST_F(MobileExportRunnerTests, IOSValidatesAppBundleAndHost)
{
    Settings.platformName = "iOS";
    Script("(out/'Game.app').mkdir(exist_ok=True)\n"
           "(out/'Game.app/Info.plist').write_text('plist')\n" + Receipt("r['requestId']", "Game.app"));
    if (CurrentBuildHost() == BuildHost::Mac)
    {
        EXPECT_TRUE(Run()) << Error;
        fs::remove(Settings.outputDirectory / "Game.app/Info.plist");
        Script(Receipt("r['requestId']", "Game.app"));
        EXPECT_FALSE(Run());
    }
    else
    {
        EXPECT_FALSE(Run());
        EXPECT_NE(Error.find("Mac"), std::string::npos);
    }
}

TEST_F(MobileExportRunnerTests, CancelledBeforeStartNeedsNoLauncher)
{
    Pipeline.RequestCancel();
    EXPECT_FALSE(Run());
    EXPECT_EQ(Error, "Mobile build cancelled.");
    EXPECT_FALSE(fs::exists(Settings.outputDirectory));
}

TEST_F(MobileExportRunnerTests, CancellationStopsLauncher)
{
    Script("time.sleep(30)\n");
    std::thread cancellation([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Pipeline.RequestCancel();
    });
    EXPECT_FALSE(Run());
    cancellation.join();
    EXPECT_TRUE(Pipeline.IsCancelled());
    EXPECT_NE(Error.find("cancelled"), std::string::npos);
}
}
