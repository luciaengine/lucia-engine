#include "Assets/AssetManager.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/ModelAssetSettings.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Core/Engine.h"
#include "Engine/Build/BuildPipeline.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;
namespace
{
void WriteFile(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good()) << path;
}
struct ExpectedSetting
{
    const char* Asset;
    const char* Key;
    std::string Value;
};
class PackagedAssetManifestTest : public testing::Test
{
protected:
    void SetUp() override
    {
        WriteFile(Assets / "Model.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        WriteFile(Assets / "Low.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        WriteFile(Assets / "Image.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16"><rect width="32" height="16" fill="red"/></svg>)");
        const std::array<unsigned char, 46> wave = {
            'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
            1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0,'d','a','t','a',2,0,0,0,0,0};
        WriteFile(Assets / "Sound.wav", {reinterpret_cast<const char*>(wave.data()), wave.size()});
        // Opaque mandatory dependencies: this export test does not render or play.
        WriteFile(Assets / "Fonts/Roboto-Regular.ttf", "TTFDATA");
        WriteFile(Assets / kRuntimeHumanoidProfilePath, "{}");
        // The public prebuilt-runtime route stages this header without launching it.
        std::array<char, 20> elf{};
        elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        elf[4] = 2; elf[5] = 1; elf[18] = 62;
        WriteFile(Runtime / "Player", {elf.data(), elf.size()});
        ApplicationConfig config;
        config.WorkspaceDirectory = Project.string();
        config.AssetDirectory = Assets.string();
        config.AssetDatabaseFile = (Project / "AssetDatabase.assetdb").string();
        config.AssetDatabaseCacheDirectory = (Project / ".Cache/AssetDatabase").string();
        ASSERT_TRUE(Engine.Initialize(config));
        Engine.GetAssetManager().WaitForStartupScan();
    }
    void TearDown() override
    {
        Engine.Shutdown();
        fs::current_path(PreviousDirectory);
    }
    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("packaged_asset_manifest")};
    const fs::path PreviousDirectory = fs::current_path();
    const fs::path Project = Temporary.Path() / "Project";
    const fs::path Assets = Project / "Assets";
    const fs::path Runtime = Temporary.Path() / "Runtime";
    const fs::path Output = Temporary.Path() / "Export";
    EngineCore Engine;
};
} // namespace

TEST_F(PackagedAssetManifestTest, RuntimeSettingsSurviveExportAndPlayerManifestMount)
{
    auto& registry = Engine.GetAssetManager().GetRegistry();
    const auto lowGuid = registry.GetAssetGUID(Assets / "Low.obj");
    ASSERT_FALSE(lowGuid.IsNull());
    const auto slot = EncodeLodSlotValue("Low.obj", lowGuid);
    const std::vector<ExpectedSetting> settings = {
        {"Sound.wav", "audio.loadPolicy", "Stream"},
        {"Sound.wav", "audio.allowVirtualization", "false"},
        {"Model.obj", "assets.lod.slot1", slot},
        {"Model.obj", "assets.lod.slot2", slot},
        {"Model.obj", "assets.lod.slot3", slot},
        {"Model.obj", "assets.model.rigKind", "none"},
        {"Model.obj", "assets.model.boneLOD", R"({"budget":24,"mandatoryPaths":[["Root","Hand"]]})"},
        {"Image.svg", "assets.texture.svgRasterSize", "64"},
        {"Model.obj", "assets.lod.generate", "false"},
        {"Image.svg", "assets.texture.filter", "point"},
    };
    for (const auto& setting : settings)
        ASSERT_TRUE(registry.SetMetaValue(Assets / setting.Asset, setting.Key, setting.Value)) << setting.Key;
    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "ManifestProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    ASSERT_TRUE(fs::exists(Output / "Assets/.assetmanifest"));
    AssetManager playerAssets;
    ASSERT_TRUE(playerAssets.Initialize(Output / "Assets", nullptr,
                                         Temporary.Path() / "UserData/AssetDatabase.assetdb",
                                         Temporary.Path() / "UserData/.Cache/AssetDatabase"));
    const auto& playerRegistry = playerAssets.GetRegistry();
    EXPECT_FALSE(playerRegistry.AcceptsDerivedRecords(Output / "Assets/Model.obj"));
    for (const auto& setting : settings)
    {
        SCOPED_TRACE(setting.Key);
        std::string value;
        EXPECT_TRUE(playerRegistry.TryGetMetaValue(Output / "Assets" / setting.Asset, setting.Key, value));
        EXPECT_EQ(value, setting.Value);
    }
    EXPECT_EQ(ModelAssetSettings::Load(playerRegistry, Output / "Assets/Model.obj").RigKind,
              ModelRigKind::NotHumanoid);
    const auto boneLOD = BoneLODAssetSettings::Load(playerRegistry, Output / "Assets/Model.obj");
    EXPECT_TRUE(boneLOD.Error.empty());
    EXPECT_EQ(boneLOD.BoneBudget, 24u);
    EXPECT_EQ(boneLOD.MandatoryPaths, (std::vector<std::vector<std::string>>{{"Root", "Hand"}}));
    playerAssets.Shutdown();
}

namespace
{
class ForestStandExportTest : public PackagedAssetManifestTest
{
protected:
    static constexpr float kTreeBoundRadius = 4.0f;

    static void WriteArchive(const fs::path& path)
    {
        using PageStreaming::PackedPlacement;
        const std::array records{PackedPlacement{20, 40, 0, 0, 128}, PackedPlacement{80, 100, 24, 64, 255}};
        const std::array pages{PageStreaming::PlacementPage{
            .Origin = {4'000.0, 10.0, -6'000.0}, .LocalMin = {-6, -5, -6}, .LocalMax = {8, 15, 8},
            .First = 0, .Count = static_cast<uint32>(records.size())}};
        fs::create_directories(path.parent_path());
        ASSERT_EQ(PageStreaming::WritePackedPlacementStore(path, kTreeBoundRadius, pages, records), "");
    }

    // Registers an asset written after the startup scan and returns its GUID.
    GUID Register(const fs::path& path)
    {
        auto& registry = Engine.GetAssetManager().GetRegistry();
        EXPECT_TRUE(registry.RegisterAsset(path)) << path;
        return registry.GetAssetGUID(path);
    }

    // Exports Scenes/Forest.scene with one ForestStand of `archive` and `treeModel`.
    bool Export(const GUID& archive, const GUID& treeModel, BuildProgress& progress)
    {
        const fs::path scene = Assets / "Scenes/Forest.scene";
        std::string body = "[scene name=\"Forest\" version=1]\n\n[entity id=\"e_stand\"]\n"
                           "ForestStand.Archive = \"" + archive.ToString() + "\"\n";
        if (!treeModel.IsNull())
            body += "ForestStand.TreeModel = \"" + treeModel.ToString() + "\"\n";
        WriteFile(scene, body);
        Register(scene);

        BuildSettings build;
        build.platformName = "Linux";
        build.projectRoot = Project;
        build.prebuiltPlayerDirectory = Runtime;
        build.outputDirectory = Output;
        build.compileScripts = false;
        build.buildConfiguration = "Release";
        build.playerConfig.gameName = "ForestExportProbe";
        build.scenes = {"Scenes/Forest.scene"};
        BuildPipeline pipeline(nullptr);
        return pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; });
    }

    // Stages Scenes/Forest.scene with one ForestStand of `archive` for a mobile export.
    bool StageMobile(const GUID& archive, std::string& error)
    {
        const fs::path scene = Assets / "Scenes/Forest.scene";
        WriteFile(scene, "[scene name=\"Forest\" version=1]\n\n[entity id=\"e_stand\"]\n"
                         "ForestStand.Archive = \"" + archive.ToString() + "\"\n");
        Register(scene);
        BuildSettings build;
        build.platformName = "iOS";
        build.projectRoot = Project;
        build.outputDirectory = Output;
        build.playerConfig.gameName = "ForestExportProbe";
        build.scenes = {"Scenes/Forest.scene"};
        BuildPipeline pipeline(nullptr);
        fs::path contentRoot;
        return pipeline.PrepareMobileContent(build, contentRoot, error);
    }

    static bool HasError(const BuildProgress& progress, std::string_view text)
    {
        return std::ranges::any_of(progress.errors,
            [text](const std::string& error) { return error.find(text) != std::string::npos; });
    }
};
} // namespace

TEST_F(ForestStandExportTest, ASceneStandShipsItsArchiveAndTreeModel)
{
    WriteArchive(Assets / "Forests/Stand.geforest");
    const GUID archive = Register(Assets / "Forests/Stand.geforest");
    const GUID model = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Assets / "Model.obj");
    ASSERT_FALSE(model.IsNull());
    BuildProgress progress;
    ASSERT_TRUE(Export(archive, model, progress)) << testing::PrintToString(progress.errors);

    PageStreaming::PackedPlacementStore store;
    ASSERT_EQ(store.Open(Output / "Assets/Forests/Stand.geforest"), "");
    ASSERT_EQ(store.Pages().size(), 1u);
    EXPECT_EQ(store.Pages().front().Count, 2u);
    EXPECT_TRUE(fs::exists(Output / "Assets/Model.obj"));
}

TEST_F(ForestStandExportTest, RefusesACorruptArchive)
{
    WriteFile(Assets / "Forests/Broken.geforest", "not a forest");
    const GUID archive = Register(Assets / "Forests/Broken.geforest");
    BuildProgress progress;
    EXPECT_FALSE(Export(archive, {}, progress));
    EXPECT_TRUE(HasError(progress, "Scene 'Scenes/Forest.scene', entity 'e_stand': the Forest Stand archive 'Forests/Broken.geforest' is not valid"))
        << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets/Forests/Broken.geforest"));
}

TEST_F(ForestStandExportTest, AMobileExportRefusesACorruptArchive)
{
    WriteFile(Assets / "Forests/Broken.geforest", "not a forest");
    const GUID archive = Register(Assets / "Forests/Broken.geforest");
    std::string error;
    EXPECT_FALSE(StageMobile(archive, error));
    EXPECT_NE(error.find("Scene 'Scenes/Forest.scene', entity 'e_stand': the Forest Stand archive "
                         "'Forests/Broken.geforest' is not valid"),
              std::string::npos)
        << error;
}

TEST_F(ForestStandExportTest, RefusesAnArchiveLinkedFromOutsideTheAssetFolder)
{
    WriteArchive(Temporary.Path() / "Outside/Stand.geforest");
    fs::create_directories(Assets / "Forests");
    std::error_code error;
    fs::create_symlink(Temporary.Path() / "Outside/Stand.geforest", Assets / "Forests/Linked.geforest", error);
    if (error)
        GTEST_SKIP() << "This file system cannot make a symbolic link: " << error.message();
    const GUID archive = Register(Assets / "Forests/Linked.geforest");
    BuildProgress progress;
    EXPECT_FALSE(Export(archive, {}, progress));
    EXPECT_TRUE(HasError(progress, "Scene 'Scenes/Forest.scene', entity 'e_stand': the Forest Stand archive"));
    EXPECT_TRUE(HasError(progress, "resolves outside its asset folder. Replace the link with the archive file."))
        << testing::PrintToString(progress.errors);
}

TEST_F(ForestStandExportTest, RefusesAnArchiveThatIsNotAForestFile)
{
    const GUID model = Engine.GetAssetManager().GetRegistry().GetAssetGUID(Assets / "Model.obj");
    ASSERT_FALSE(model.IsNull());
    BuildProgress progress;
    EXPECT_FALSE(Export(model, {}, progress));
    EXPECT_TRUE(HasError(progress, "Scene 'Scenes/Forest.scene', entity 'e_stand': the Forest Stand archive "
                                   "'Model.obj' is not a .geforest file."))
        << testing::PrintToString(progress.errors);
}

TEST_F(ForestStandExportTest, RefusesATreeModelThatDoesNotShip)
{
    WriteArchive(Assets / "Forests/Stand.geforest");
    const GUID archive = Register(Assets / "Forests/Stand.geforest");
    BuildProgress progress;
    EXPECT_FALSE(Export(archive, GUID::Generate(), progress));
    EXPECT_TRUE(HasError(progress, "Scene 'Scenes/Forest.scene', entity 'e_stand': the Forest Stand tree model does not ship. Set Tree Model"))
        << testing::PrintToString(progress.errors);
}
