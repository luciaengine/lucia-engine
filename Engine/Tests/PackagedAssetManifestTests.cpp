#include "Assets/AssetManager.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/HlodBakeDriver.h"
#include "Assets/HlodCache.h"
#include "Assets/ModelAssetSettings.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Core/Engine.h"
#include "Engine/Build/BuildPipeline.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
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
        {"Model.obj", "assets.model.impostor", "true"},
        {"Model.obj", "assets.model.impostorScreenHeightPercent", "12"},
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
    const auto modelSettings = ModelAssetSettings::Load(playerRegistry, Output / "Assets/Model.obj");
    EXPECT_EQ(modelSettings.RigKind, ModelRigKind::NotHumanoid);
    EXPECT_TRUE(modelSettings.Impostor);
    EXPECT_EQ(modelSettings.ImpostorScreenHeightPercent, 12u);
    const auto linkedLodSettings = ModelAssetSettings::Load(playerRegistry, Output / "Assets/Low.obj");
    EXPECT_FALSE(linkedLodSettings.Impostor);
    EXPECT_EQ(linkedLodSettings.ImpostorScreenHeightPercent, 0u);
    const auto boneLOD = BoneLODAssetSettings::Load(playerRegistry, Output / "Assets/Model.obj");
    EXPECT_TRUE(boneLOD.Error.empty());
    EXPECT_EQ(boneLOD.BoneBudget, 24u);
    EXPECT_EQ(boneLOD.MandatoryPaths, (std::vector<std::vector<std::string>>{{"Root", "Hand"}}));
    playerAssets.Shutdown();
}

TEST_F(PackagedAssetManifestTest, ExportKeepsSeparateHlodBakesForSameNamedScenes)
{
    auto& assets = Engine.GetAssetManager();
    auto& registry = assets.GetRegistry();
    const std::array<fs::path, 2> scenes{ "Scenes/Forest/Overview.scene", "Scenes/Mountain/Overview.scene" };
    std::array<GUID, 2> sceneIds;
    for (size_t i = 0; i < scenes.size(); ++i)
    {
        const fs::path scene = Assets / scenes[i];
        WriteFile(scene, "[scene name=\"Overview\" version=1]\n");
        ASSERT_TRUE(registry.RegisterAsset(scene));
        sceneIds[i] = registry.GetAssetGUID(scene);
        ASSERT_FALSE(sceneIds[i].IsNull());
        Hlod::HlodBakedScene bake{};
        bake.Key.ConfigHash = i + 1;
        bake.CellSize = 64.0f;
        ASSERT_TRUE(Hlod::WriteHlodCache(Hlod::ResolveHlodCacheFile(assets, scene), bake));
    }
    ASSERT_NE(sceneIds[0], sceneIds[1]);

    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "HlodSidecarProbe";
    for (const auto& scene : scenes)
        build.scenes.push_back(scene.generic_string());
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);

    AssetManager playerAssets;
    ASSERT_TRUE(playerAssets.Initialize(Output / "Assets", nullptr,
                                        Temporary.Path() / "UserData/AssetDatabase.assetdb",
                                        Temporary.Path() / "UserData/.Cache/AssetDatabase"));
    for (size_t i = 0; i < scenes.size(); ++i)
    {
        const GUID packagedId = playerAssets.GetRegistry().GetAssetGUID(Output / "Assets" / scenes[i]);
        ASSERT_EQ(packagedId, sceneIds[i]);
        const fs::path sidecar = Hlod::PackagedHlodCacheFile(playerAssets.GetAssetRoot(), packagedId);
        Hlod::HlodBakedScene bake;
        ASSERT_EQ(Hlod::ReadHlodCache(sidecar, nullptr, bake), Hlod::HlodCacheStatus::Hit);
        EXPECT_EQ(bake.Key.ConfigHash, i + 1);
    }
    playerAssets.Shutdown();
}

TEST_F(PackagedAssetManifestTest, ForestStandsAndNearBudgetsSurviveDesktopExport)
{
    const std::string archive = "Forests/StandingTrees.geforest";
    const auto sourceArchive = Assets / archive;
    fs::create_directories(sourceArchive.parent_path());
    using PageStreaming::PackedPlacement;
    const std::array records{PackedPlacement{20, 40, 0, 32768}, PackedPlacement{80, 100, 16384, 65535}};
    const std::array pages{PageStreaming::PlacementPage{
        .Origin = {4'000.0, 10.0, -6'000.0}, .LocalMin = {-2, 0, -2}, .LocalMax = {8, 15, 8},
        .First = 0, .Count = static_cast<uint32>(records.size())}};
    ASSERT_EQ(PageStreaming::WritePackedPlacementStore(sourceArchive, pages, records), "");

    const std::string secondArchive = "Forests/OtherTrees.geforest";
    auto secondPage = pages.front();
    secondPage.Count = 1;
    ASSERT_EQ(PageStreaming::WritePackedPlacementStore(Assets / secondArchive, std::span(&secondPage, 1u),
                                                        std::span(records).first(1)), "");

    GameConfig sourceConfig;
    sourceConfig.forestStands.push_back({
        .archive = archive, .entity = "ForestOverview", .impostorModel = "Model.obj",
        .detailDistance = 275.0f, .nearMeshBudget = 96});
    sourceConfig.forestStands.push_back({.archive = secondArchive, .entity = "OtherOverview"});
    ASSERT_TRUE(SaveGameConfig(Project / "game.config", sourceConfig));

    BuildSettings build;
    build.platformName = "Linux";
    build.projectRoot = Project;
    build.prebuiltPlayerDirectory = Runtime;
    build.outputDirectory = Output;
    build.compileScripts = false;
    build.buildConfiguration = "Release";
    build.playerConfig.gameName = "ForestExportProbe";
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(build, [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);

    const auto packaged = LoadGameConfig(Output / "game.config");
    ASSERT_EQ(packaged.forestStands.size(), 2u);
    EXPECT_EQ(packaged.forestStands[1].archive, secondArchive);
    EXPECT_EQ(packaged.forestStands[1].entity, "OtherOverview");
    EXPECT_EQ(packaged.forestStands[1].nearMeshBudget, 0u);
    PageStreaming::PackedPlacementStore exportedSecond;
    ASSERT_EQ(exportedSecond.Open(Output / "Assets" / secondArchive), "");
    EXPECT_EQ(exportedSecond.PlacementCount(), 1u);
    EXPECT_EQ(packaged.forestStands.front().archive, archive);
    EXPECT_EQ(packaged.forestStands.front().entity, "ForestOverview");
    EXPECT_EQ(packaged.forestStands.front().impostorModel, "Model.obj");
    EXPECT_FLOAT_EQ(packaged.forestStands.front().detailDistance, 275.0f);
    EXPECT_EQ(packaged.forestStands.front().nearMeshBudget, 96u);

    PageStreaming::PackedPlacementStore store;
    ASSERT_EQ(store.Open(Output / "Assets" / packaged.forestStands.front().archive), "");
    ASSERT_EQ(store.Pages().size(), 1u);
    EXPECT_EQ(store.PlacementCount(), records.size());
    EXPECT_EQ(store.Pages().front().Origin, pages.front().Origin);
    const auto range = store.ReadRange(0);
    ASSERT_TRUE(range);
    ASSERT_EQ(range->Bytes, sizeof(records));
    std::array<uint8, sizeof(records)> bytes{};
    std::ifstream data(store.File(), std::ios::binary);
    data.seekg(static_cast<std::streamoff>(range->Offset));
    ASSERT_TRUE(data.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    EXPECT_TRUE(store.VerifyPage(0, bytes));
}
