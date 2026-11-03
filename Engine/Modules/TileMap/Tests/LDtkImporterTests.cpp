#include "TileMap/Importers/LDtkImporter.h"

#include "TestPaths.h"
#include "TestTempDir.h"
#include "TileMap/TileCellEncoding.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

using namespace GameEngine;
using namespace GameEngine::TileMap;
using namespace GameEngine::TileMap::Importers;
using GameEngine::TileMapTests::FixtureDir;

namespace
{

constexpr const char* kAtlasGuidText = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";

TextureGuidResolver FixedResolver()
{
    return [](const std::filesystem::path&) { return GUID(kAtlasGuidText); };
}

std::filesystem::path WriteTemp(const std::string& name, const std::string& contents)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("LDtkImporterTests_" + name);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return path;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool HasWarningContaining(const TileMapImportResult& result, const std::string& needle)
{
    for (const std::string& warning : result.Warnings)
        if (warning.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST(LDtkImporterTests, BasicProjectImportsTileSetAndLevel)
{
    const TileMapImportResult result =
        ImportLDtkProject(FixtureDir() / "basic.ldtk", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;

    ASSERT_EQ(result.TileSets.size(), 1u);
    const ImportedTileSet& tileSet = result.TileSets[0];
    EXPECT_EQ(tileSet.Name, "Terrain");
    EXPECT_EQ(tileSet.ImagePath.filename(), "tiles.png");
    ASSERT_NE(tileSet.Asset, nullptr);
    EXPECT_EQ(tileSet.Asset->AtlasTexture(), GUID(kAtlasGuidText));
    EXPECT_EQ(tileSet.Asset->Columns(), 4);
    EXPECT_EQ(tileSet.Asset->Rows(), 2);
    EXPECT_EQ(tileSet.Asset->Tiles().size(), 8u);

    // LDtk customData tileId 0 lands on our 1-based tile 1; enum tags become
    // boolean custom values on tiles 2 and 3.
    const TileSetTile* tile1 = tileSet.Asset->FindTile(1);
    ASSERT_NE(tile1, nullptr);
    ASSERT_TRUE(tile1->CustomData.contains("data"));
    EXPECT_EQ(std::get<std::string>(tile1->CustomData.at("data")), "grass");
    const TileSetTile* tile2 = tileSet.Asset->FindTile(2);
    ASSERT_NE(tile2, nullptr);
    ASSERT_TRUE(tile2->CustomData.contains("Solid"));
    EXPECT_TRUE(std::get<bool>(tile2->CustomData.at("Solid")));

    ASSERT_EQ(result.Maps.size(), 1u);
    const ImportedTileMap& map = result.Maps[0];
    EXPECT_EQ(map.Name, "basic_Level_0");
    ASSERT_NE(map.Asset, nullptr);
    const TileMapData& data = map.Asset->Data();
    ASSERT_EQ(data.Layers().size(), 3u);

    // Tiles layer: LDtk rows flip so the level's bottom row is Y = 0, and the
    // f bits map to FlipH/FlipV.
    const TileMapLayerData& tiles = data.Layers()[0];
    EXPECT_EQ(tiles.Name, "Tiles");
    EXPECT_EQ(tiles.Type, TileMapLayerType::Tiles);
    EXPECT_EQ(tiles.TileSetGuid, tileSet.Guid);
    EXPECT_EQ(tiles.GetCell(0, 2), MakeCell(1));
    EXPECT_EQ(tiles.GetCell(1, 2), MakeCell(2, /*flipH*/ true));
    EXPECT_EQ(tiles.GetCell(0, 0), MakeCell(7, false, /*flipV*/ true));
    EXPECT_EQ(tiles.GetCell(3, 0), MakeCell(8, true, true));

    // IntGrid layer: values plus the def table.
    const TileMapLayerData& ground = data.Layers()[1];
    EXPECT_EQ(ground.Name, "Ground");
    EXPECT_EQ(ground.Type, TileMapLayerType::IntGrid);
    ASSERT_EQ(ground.IntGridValues.size(), 2u);
    EXPECT_EQ(ground.IntGridValues[0].Name, "dirt");
    EXPECT_EQ(ground.IntGridValues[0].Value, 1);
    EXPECT_EQ(ground.IntGridValues[0].ColorRGBA, 0xAA5500FFu);
    EXPECT_EQ(ground.GetIntGridValue(0, 2), 1);
    EXPECT_EQ(ground.GetIntGridValue(3, 2), 2);
    EXPECT_EQ(ground.GetIntGridValue(0, 0), 1);
    EXPECT_EQ(ground.GetIntGridValue(1, 0), 1);
    EXPECT_EQ(ground.GetIntGridValue(2, 0), 0);

    // Dressed auto-layer output: two tiles stacked on one cell keep the
    // topmost (last in the array).
    const TileMapLayerData& dressed = data.Layers()[2];
    EXPECT_EQ(dressed.Name, "Ground (tiles)");
    EXPECT_EQ(dressed.Type, TileMapLayerType::Tiles);
    EXPECT_EQ(dressed.GetCell(0, 2), MakeCell(6));
    EXPECT_TRUE(HasWarningContaining(result, "kept the topmost"));
}

TEST(LDtkImporterTests, GoldenMapSerialization)
{
    const TileMapImportResult result =
        ImportLDtkProject(FixtureDir() / "basic.ldtk", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.Maps.size(), 1u);

    // The golden is built through the same authoring API a hand-made map uses,
    // so the comparison pins the importer to the canonical serialized form.
    TileMapAsset expected(GUID::Generate(), "golden.tilemap");
    TileMapData& data = expected.Data();
    const uint32 tilesId = data.AddLayer("Tiles", TileMapLayerType::Tiles);
    TileMapLayerData& tiles = *data.FindLayer(tilesId);
    tiles.TileSetGuid = result.TileSets[0].Guid;
    tiles.SetCell(0, 2, MakeCell(1));
    tiles.SetCell(1, 2, MakeCell(2, true));
    tiles.SetCell(0, 0, MakeCell(7, false, true));
    tiles.SetCell(3, 0, MakeCell(8, true, true));

    const uint32 groundId = data.AddLayer("Ground", TileMapLayerType::IntGrid);
    TileMapLayerData& ground = *data.FindLayer(groundId);
    ground.IntGridValues.push_back(IntGridValueDef{1, "dirt", 0xAA5500FFu});
    ground.IntGridValues.push_back(IntGridValueDef{2, "water", 0x0055AAFFu});
    ground.SetIntGridValue(0, 2, 1);
    ground.SetIntGridValue(3, 2, 2);
    ground.SetIntGridValue(0, 0, 1);
    ground.SetIntGridValue(1, 0, 1);

    const uint32 dressedId = data.AddLayer("Ground (tiles)", TileMapLayerType::Tiles);
    TileMapLayerData& dressed = *data.FindLayer(dressedId);
    dressed.TileSetGuid = result.TileSets[0].Guid;
    dressed.SetCell(0, 2, MakeCell(6));

    Vector<uint8> expectedBytes;
    Vector<uint8> importedBytes;
    ASSERT_TRUE(expected.SaveToData(expectedBytes));
    ASSERT_TRUE(result.Maps[0].Asset->SaveToData(importedBytes));
    EXPECT_EQ(std::string(importedBytes.begin(), importedBytes.end()),
              std::string(expectedBytes.begin(), expectedBytes.end()));
}

TEST(LDtkImporterTests, ExternalLevelFileImports)
{
    const TileMapImportResult result =
        ImportLDtkProject(FixtureDir() / "external_levels.ldtk", FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.Maps.size(), 1u);
    const TileMapData& data = result.Maps[0].Asset->Data();
    ASSERT_EQ(data.Layers().size(), 1u);
    // px (16,16) in a 2x2 level: cell (1, row 1) -> engine (1, 0).
    EXPECT_EQ(data.Layers()[0].GetCell(1, 0), MakeCell(2));
}

TEST(LDtkImporterTests, MissingFileFails)
{
    const TileMapImportResult result =
        ImportLDtkProject(FixtureDir() / "does_not_exist.ldtk", FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("cannot read"), std::string::npos);
}

TEST(LDtkImporterTests, TruncatedProjectFailsCleanly)
{
    const std::string full = ReadAll(FixtureDir() / "basic.ldtk");
    ASSERT_GT(full.size(), 100u);
    const auto path = WriteTemp("truncated.ldtk", full.substr(0, full.size() / 2));
    const TileMapImportResult result = ImportLDtkProject(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("malformed JSON"), std::string::npos);
    EXPECT_TRUE(result.Maps.empty());
    EXPECT_TRUE(result.TileSets.empty());
}

TEST(LDtkImporterTests, NotAProjectFails)
{
    const auto path = WriteTemp("empty.ldtk", "{}");
    const TileMapImportResult result = ImportLDtkProject(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("no defs"), std::string::npos);
}

TEST(LDtkImporterTests, HostileHugeLayerBoundsSkippedNotAllocated)
{
    // A layer claiming absurd bounds must be skipped by the cap, not sized.
    const std::string project = R"({
      "defs": { "tilesets": [ { "uid": 1, "identifier": "T", "relPath": "t.png",
                 "pxWid": 64, "pxHei": 32, "tileGridSize": 16, "spacing": 0, "padding": 0 } ],
                "layers": [] },
      "levels": [ { "identifier": "L",
        "layerInstances": [ { "__identifier": "Big", "__type": "Tiles",
          "__cWid": 2000000000, "__cHei": 2000000000, "__gridSize": 16,
          "__tilesetDefUid": 1, "gridTiles": [] } ] } ]
    })";
    const auto path = WriteTemp("huge.ldtk", project);
    const TileMapImportResult result = ImportLDtkProject(path, FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.Maps.size(), 1u);
    EXPECT_TRUE(result.Maps[0].Asset->Data().Layers().empty());
    for (const std::string& warning : result.Warnings)
    {
        if (warning.find("oversized") != std::string::npos)
            return;
    }
    FAIL() << "expected an oversized-bounds warning";
}

TEST(LDtkImporterTests, HostileIntGridValueOutsideByteRangeSkipped)
{
    const std::string project = R"({
      "defs": { "tilesets": [], "layers": [] },
      "levels": [ { "identifier": "L",
        "layerInstances": [ { "__identifier": "G", "__type": "IntGrid",
          "__cWid": 2, "__cHei": 1, "__gridSize": 16,
          "intGridCsv": [70000, -3] } ] } ]
    })";
    const auto path = WriteTemp("intgrid_range.ldtk", project);
    const TileMapImportResult result = ImportLDtkProject(path, FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    const TileMapData& data = result.Maps[0].Asset->Data();
    ASSERT_EQ(data.Layers().size(), 1u);
    EXPECT_EQ(data.Layers()[0].GetIntGridValue(0, 0), 0);
    EXPECT_EQ(data.Layers()[0].GetIntGridValue(1, 0), 0);
}

TEST(LDtkImporterTests, ExternalLevelOutsideTheProjectDirectoryIsSkipped)
{
    const auto path = WriteTemp(
        "escape_level.ldtk",
        R"({"defs":{"tilesets":[],"layers":[]},)"
        R"("levels":[{"identifier":"L","externalRelPath":"../../../../../../etc/hosts"}]})");
    const TileMapImportResult result = ImportLDtkProject(path, FixedResolver());
    EXPECT_TRUE(result.Success) << result.Error;
    EXPECT_TRUE(HasWarningContaining(result, "outside")) << "expected a containment warning";
}

TEST(LDtkImporterTests, DuplicateTileSetUidsAreRejected)
{
    // Layers bind to a tileset by uid; a second definition under the same uid
    // (or several defaulting to the same missing uid) binds them to the wrong one.
    const auto path = WriteTemp(
        "dupe_uid.ldtk",
        R"({"defs":{"tilesets":[)"
        R"({"uid":7,"identifier":"A","relPath":"a.png","pxWid":64,"pxHei":32,"tileGridSize":16},)"
        R"({"uid":7,"identifier":"B","relPath":"b.png","pxWid":64,"pxHei":32,"tileGridSize":16})"
        R"(],"layers":[]},"levels":[]})");
    const TileMapImportResult result = ImportLDtkProject(path, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("uid"), std::string::npos) << result.Error;
}

TEST(LDtkImporterTests, WrongFieldTypesFailWithoutThrowing)
{
    // The importer must not depend on exceptions: catch(const std::exception&)
    // is dead in builds whose translation units disagree about RTTI.
    const char* const hostile[] = {
        R"({"defs":{"tilesets":[{"uid":"7"}],"layers":[]},"levels":[]})",
        R"({"defs":{"tilesets":[{"uid":7,"identifier":9}],"layers":[]},"levels":[]})",
        R"({"defs":{"tilesets":[{"uid":7,"relPath":"a.png","pxWid":"64"}],"layers":[]},"levels":[]})",
        R"({"defs":{"tilesets":[],"layers":[{"uid":"1","intGridValues":[{"value":"3"}]}]},"levels":[]})",
        R"({"defs":{"tilesets":[],"layers":[{"uid":1,"intGridValues":[{"value":3,"color":7}]}]},"levels":[]})",
        R"({"defs":{"tilesets":[],"layers":[]},"levels":[{"identifier":5,"layerInstances":[]}]})",
        R"({"defs":{"tilesets":[],"layers":[]},"levels":[{"layerInstances":[{"__type":9}]}]})",
        R"({"defs":{"tilesets":[],"layers":[]},"levels":[{"layerInstances":)"
        R"([{"__type":"Tiles","__cWid":"4","__cHei":4,"__gridSize":16}]}]})",
        R"({"defs":{"tilesets":[{"uid":1,"identifier":"A","relPath":"a.png","pxWid":64,)"
        R"("pxHei":32,"tileGridSize":16}],"layers":[]},"levels":[{"layerInstances":)"
        R"([{"__type":"Tiles","__identifier":"L","__cWid":4,"__cHei":2,"__gridSize":16,)"
        R"("__tilesetDefUid":1,"gridTiles":[{"px":["0","0"],"src":[0,0]}]}]}]})",
        R"({"defs":{"tilesets":[{"uid":1,"identifier":"A","relPath":"a.png","pxWid":64,)"
        R"("pxHei":32,"tileGridSize":16}],"layers":[]},"levels":[{"layerInstances":)"
        R"([{"__type":"Tiles","__identifier":"L","__cWid":4,"__cHei":2,"__gridSize":16,)"
        R"("__tilesetDefUid":1,"gridTiles":[{"px":[0,0],"src":[0,0],"f":"1"}]}]}]})",
        R"({"defs":{"tilesets":[],"layers":[]},"levels":[{"layerInstances":)"
        R"([{"__type":"IntGrid","__cWid":2,"__cHei":1,"__gridSize":16,"layerDefUid":"1",)"
        R"("intGridCsv":[1,2]}]}]})",
    };
    for (const char* text : hostile)
    {
        const auto path = WriteTemp("types.ldtk", text);
        EXPECT_NO_THROW({ (void)ImportLDtkProject(path, FixedResolver()); }) << text;
    }
}

TEST(LDtkImporterTests, ATruncatedExternalLevelIsSkippedNotFatal)
{
    const auto levelPath = WriteTemp("broken_level.ldtkl", R"({"identifier":"L",)");
    const auto path = WriteTemp(
        "external_broken.ldtk",
        R"({"defs":{"tilesets":[],"layers":[]},"levels":[{"identifier":"L","externalRelPath":")" +
            levelPath.filename().string() + R"("}]})");
    const TileMapImportResult result = ImportLDtkProject(path, FixedResolver());
    EXPECT_TRUE(result.Success) << result.Error;
    EXPECT_TRUE(HasWarningContaining(result, "malformed JSON"));
}

TEST(LDtkImporterTests, ExtremeCoordinatesAreSkippedBeforeSignedArithmetic)
{
    auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
    project["defs"]["tilesets"][0]["padding"] = 1;
    auto& layer = project["levels"][0]["layerInstances"][0];
    layer["gridTiles"] = nlohmann::json::parse(R"([
        {"px":[0,0],"src":[-9223372036854775808,1]},
        {"px":[0,0],"src":[9223372036854775808,1]},
        {"px":[18446744073709551615,0],"src":[1,1]},
        {"px":[16,0],"src":[1,1]}
    ])");
    const auto result = ImportLDtkProject(WriteTemp("extreme_coordinates.ldtk", project.dump()), FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_FALSE(result.Maps.empty());
    const auto& cells = result.Maps[0].Asset->Data().Layers()[0];
    EXPECT_EQ(cells.GetCell(0, 2), 0u);
    EXPECT_EQ(cells.GetCell(1, 2), MakeCell(1));
    EXPECT_TRUE(HasWarningContaining(result, "skipped 3"));
}

TEST(LDtkImporterTests, UnsignedTileSetUidDoesNotWrapToNegativeDefinition)
{
    auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
    project["defs"]["tilesets"][0]["uid"] = -1;
    auto layer = project["levels"][0]["layerInstances"][0];
    layer["__tilesetDefUid"] = UINT64_MAX;
    project["levels"][0]["layerInstances"] = nlohmann::json::array({layer});
    const auto result = ImportLDtkProject(WriteTemp("unsigned_uid.ldtk", project.dump()), FixedResolver());
    ASSERT_TRUE(result.Success) << result.Error;
    ASSERT_EQ(result.Maps.size(), 1u);
    EXPECT_TRUE(result.Maps[0].Asset->Data().Layers().empty());
    EXPECT_TRUE(HasWarningContaining(result, "no importable tileset"));
}

TEST(LDtkImporterTests, CollidingOutputNamesRejectTheWholeImport)
{
    for (const auto& names : {std::pair{"A/B", "A:B"}, std::pair{"Terrain", "terrain"}})
    {
        auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
        auto second = project["defs"]["tilesets"][0];
        project["defs"]["tilesets"][0]["identifier"] = names.first;
        second["identifier"] = names.second;
        second["uid"] = 9999;
        project["defs"]["tilesets"].push_back(second);
        const auto result = ImportLDtkProject(WriteTemp("name_collision.ldtk", project.dump()), FixedResolver());
        EXPECT_FALSE(result.Success);
        EXPECT_NE(result.Error.find("duplicate output filename"), std::string::npos);
        EXPECT_TRUE(result.TileSets.empty());
        EXPECT_TRUE(result.Maps.empty());
    }
    auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
    auto second = project["levels"][0];
    project["levels"][0]["identifier"] = "Upper";
    second["identifier"] = "upper";
    project["levels"].push_back(second);
    const auto result = ImportLDtkProject(WriteTemp("level_collision.ldtk", project.dump()), FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("duplicate output filename"), std::string::npos);
    EXPECT_TRUE(result.TileSets.empty());
    EXPECT_TRUE(result.Maps.empty());
}

TEST(LDtkImporterTests, SymlinkOutsideSourceNeverReachesTextureResolverOrLevelReader)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("ldtk_symlink"));
    const auto source = temp.Path() / "source";
    const auto outside = temp.Path() / "outside";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(outside);
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, source / "link", error);
    if (error)
        GTEST_SKIP() << "Creating a directory symlink is unavailable: " << error.message();
    std::ofstream(outside / "level.ldtkl") << R"({"identifier":"Outside","layerInstances":[]})";
    const auto file = source / "source.ldtk";
    const auto countingResolver = [](int& calls) {
        return [&calls](const std::filesystem::path&) {
            ++calls;
            return GUID(kAtlasGuidText);
        };
    };

    // An atlas outside the source folder fails the import, as it does for Tiled.
    {
        auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
        project["defs"]["tilesets"][0]["relPath"] = "link/missing.png";
        std::ofstream(file, std::ios::trunc) << project.dump();
        int calls = 0;
        const auto result = ImportLDtkProject(file, countingResolver(calls));
        EXPECT_FALSE(result.Success);
        EXPECT_NE(result.Error.find("outside"), std::string::npos) << result.Error;
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(result.TileSets.empty());
        EXPECT_TRUE(result.Maps.empty());
    }

    // An external level outside it is skipped with a warning and never read.
    {
        auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
        project["levels"] = nlohmann::json::array({{{"identifier", "Outside"}, {"externalRelPath", "link/level.ldtkl"}}});
        std::ofstream(file, std::ios::trunc) << project.dump();
        int calls = 0;
        const auto result = ImportLDtkProject(file, countingResolver(calls));
        EXPECT_TRUE(result.Success) << result.Error;
        EXPECT_TRUE(result.Maps.empty());
        EXPECT_TRUE(HasWarningContaining(result, "outside"));
    }
}

TEST(LDtkImporterTests, SourceThatIsNotARegularFileIsRefusedBeforeReading)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("ldtk_not_regular"));
    const auto directory = temp.Path() / "folder.ldtk";
    std::filesystem::create_directories(directory);
    const auto result = ImportLDtkProject(directory, FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("not a regular file"), std::string::npos) << result.Error;

    // An external level that is a folder is skipped with a warning.
    std::filesystem::create_directories(temp.Path() / "level.ldtkl");
    auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
    project["levels"] = nlohmann::json::array({{{"identifier", "Folder"}, {"externalRelPath", "level.ldtkl"}}});
    const auto file = temp.Path() / "source.ldtk";
    std::ofstream(file) << project.dump();
    const auto withLevel = ImportLDtkProject(file, FixedResolver());
    EXPECT_TRUE(withLevel.Success) << withLevel.Error;
    EXPECT_TRUE(withLevel.Maps.empty());
    EXPECT_TRUE(HasWarningContaining(withLevel, "not a regular file"));
}

TEST(LDtkImporterTests, LevelsShareOneCellBudgetAndFailureDiscardsPriorOutput)
{
    auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
    project["levels"] = nlohmann::json::array();
    for (int i = 0; i < 5; ++i)
        project["levels"].push_back({{"identifier", "Level" + std::to_string(i)},
            {"layerInstances", nlohmann::json::array({{{"__type", "IntGrid"}, {"__cWid", 2048},
                {"__cHei", 2048}, {"__gridSize", 16}}})}});
    const auto result = ImportLDtkProject(WriteTemp("level_budget.ldtk", project.dump()), FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("whole-import cell budget"), std::string::npos);
    EXPECT_TRUE(result.TileSets.empty());
    EXPECT_TRUE(result.Maps.empty());
}

TEST(LDtkImporterTests, DefinitionArraysShareOneItemBudget)
{
    std::string text = R"({"defs":{"tilesets":[],"layers":[{"uid":1,"intGridValues":[)";
    for (uint64 i = 0; i < kMaxImportItems; ++i)
        text += i == 0 ? "null" : ",null";
    text += R"(]}]},"levels":[]})";
    const auto result = ImportLDtkProject(WriteTemp("item_budget.ldtk", text), FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_NE(result.Error.find("whole-import item budget"), std::string::npos);
    EXPECT_TRUE(result.TileSets.empty());
    EXPECT_TRUE(result.Maps.empty());
}

TEST(LDtkImporterTests, NonObjectExternalLevelFailureDiscardsPriorOutput)
{
    const auto levelPath = WriteTemp("array_level.ldtkl", "[]");
    auto project = nlohmann::json::parse(ReadAll(FixtureDir() / "basic.ldtk"));
    project["levels"].push_back({{"identifier", "Invalid"},
        {"externalRelPath", levelPath.filename().string()}});
    const auto result = ImportLDtkProject(WriteTemp("external_array.ldtk", project.dump()), FixedResolver());
    EXPECT_FALSE(result.Success);
    EXPECT_FALSE(result.Error.empty());
    EXPECT_TRUE(result.TileSets.empty());
    EXPECT_TRUE(result.Maps.empty());
}
