#include "TileMap/TileSetAsset.h"

#include "TileMap/TileCellEncoding.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

TileSetAsset MakeAsset()
{
    return TileSetAsset(GUID::Generate(), "test.tileset");
}

} // namespace

TEST(TileSetAssetGrid, ColumnsAndRowsWithMarginAndSpacing)
{
    // 256px atlas, 16px tiles, no margin/spacing: 16 columns.
    EXPECT_EQ(TileSetAsset::GridColumns(256, 16, 0, 0), 16);
    // 2px margin each side, 2px spacing: (256 - 4 + 2) / 18 = 14.
    EXPECT_EQ(TileSetAsset::GridColumns(256, 16, 2, 2), 14);
    // Degenerate inputs.
    EXPECT_EQ(TileSetAsset::GridColumns(0, 16, 0, 0), 0);
    EXPECT_EQ(TileSetAsset::GridColumns(256, 0, 0, 0), 0);
    EXPECT_EQ(TileSetAsset::GridColumns(8, 16, 0, 0), 0);
}

TEST(TileSetAssetGrid, ComputeTileUVMatchesPixelRects)
{
    // 128x64 atlas, 32x32 tiles, margin 0, spacing 0: cell (1,1) = px (32,32)-(64,64).
    const TileUVRect uv = TileSetAsset::ComputeTileUV(128, 64, 32, 32, 0, 0, 1, 1);
    EXPECT_FLOAT_EQ(uv.U0, 0.25f);
    EXPECT_FLOAT_EQ(uv.V0, 0.5f);
    EXPECT_FLOAT_EQ(uv.U1, 0.5f);
    EXPECT_FLOAT_EQ(uv.V1, 1.0f);
}

TEST(TileSetAssetGrid, ComputeTileUVHonorsMarginAndSpacing)
{
    // 100px atlas, 16px tiles, margin 2, spacing 2: cell 1 starts at 2 + 18 = 20px.
    const TileUVRect uv = TileSetAsset::ComputeTileUV(100, 100, 16, 16, 2, 2, 1, 0);
    EXPECT_FLOAT_EQ(uv.U0, 0.20f);
    EXPECT_FLOAT_EQ(uv.V0, 0.02f);
    EXPECT_FLOAT_EQ(uv.U1, 0.36f);
    EXPECT_FLOAT_EQ(uv.V1, 0.18f);
}

TEST(TileSetAssetSlice, AssignsRowMajorOneBasedIds)
{
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 64, 32);
    asset.SliceGrid(16, 16, 0, 0);

    EXPECT_EQ(asset.Columns(), 4);
    EXPECT_EQ(asset.Rows(), 2);
    ASSERT_EQ(asset.Tiles().size(), 8u);
    EXPECT_EQ(asset.Tiles().front().Id, 1u);
    EXPECT_EQ(asset.Tiles().back().Id, 8u);
    // Tile 6 = row 1, column 1.
    const TileSetTile* tile = asset.FindTile(6);
    ASSERT_NE(tile, nullptr);
    EXPECT_FLOAT_EQ(tile->UV.U0, 0.25f);
    EXPECT_FLOAT_EQ(tile->UV.V0, 0.5f);
}

TEST(TileSetAssetSlice, ReslicePreservesAuthoredDataById)
{
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 64, 32);
    asset.SliceGrid(16, 16, 0, 0);

    TileSetTile* tile = asset.FindTile(3);
    ASSERT_NE(tile, nullptr);
    tile->Probability = 0.25f;
    tile->CustomData.emplace("damage", TileCustomValue(int64(7)));

    asset.SliceGrid(16, 16, 0, 0);
    const TileSetTile* preserved = asset.FindTile(3);
    ASSERT_NE(preserved, nullptr);
    EXPECT_FLOAT_EQ(preserved->Probability, 0.25f);
    ASSERT_TRUE(preserved->CustomData.contains("damage"));
    EXPECT_EQ(std::get<int64>(preserved->CustomData.at("damage")), 7);
}

TEST(TileSetAssetJson, RoundTripPreservesEverything)
{
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 128, 128);
    asset.SliceGrid(16, 16, 2, 2);

    TerrainSetDef terrainSet;
    terrainSet.Name = "Grassland";
    terrainSet.MatchMode = TerrainMatchMode::Corners;
    terrainSet.Terrains.push_back({"Grass", 0xFF00FF00u});
    terrainSet.Terrains.push_back({"Dirt", 0xFF884400u});
    asset.TerrainSets().push_back(terrainSet);

    TileSetTile* tile = asset.FindTile(2);
    ASSERT_NE(tile, nullptr);
    tile->Probability = 0.5f;
    tile->CollisionPolygons.push_back({{Mathematics::Vector2(0.0f, 0.0f),
                                        Mathematics::Vector2(1.0f, 0.0f),
                                        Mathematics::Vector2(1.0f, 1.0f)}});
    tile->CollisionMode = TileCollisionMode::OneWay;
    tile->PhysicsMaterial = GUID::Generate();
    tile->AnimationFrames.push_back({3u, 0.2f});
    tile->Peering.TerrainSet = 0;
    tile->Peering.Center = 1;
    tile->Peering.Neighbors[static_cast<size_t>(PeeringSlot::Top)] = 0;
    tile->CustomData.emplace("breakable", TileCustomValue(true));
    tile->CustomData.emplace("name", TileCustomValue(std::string("crate")));

    TileBrushDef brush;
    brush.Name = "Door";
    brush.Width = 2;
    brush.Height = 1;
    brush.Cells = {5u, 6u};
    asset.Brushes().push_back(brush);

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));

    TileSetAsset loaded(GUID::Generate(), "loaded.tileset");
    ASSERT_TRUE(loaded.LoadFromData(data));

    EXPECT_EQ(loaded.AtlasTexture(), asset.AtlasTexture());
    EXPECT_EQ(loaded.Columns(), asset.Columns());
    EXPECT_EQ(loaded.Rows(), asset.Rows());
    EXPECT_EQ(loaded.Tiles().size(), asset.Tiles().size());
    ASSERT_EQ(loaded.TerrainSets().size(), 1u);
    EXPECT_EQ(loaded.TerrainSets()[0].Name, "Grassland");
    EXPECT_EQ(loaded.TerrainSets()[0].MatchMode, TerrainMatchMode::Corners);
    ASSERT_EQ(loaded.TerrainSets()[0].Terrains.size(), 2u);
    EXPECT_EQ(loaded.TerrainSets()[0].Terrains[1].Name, "Dirt");

    const TileSetTile* loadedTile = loaded.FindTile(2);
    ASSERT_NE(loadedTile, nullptr);
    EXPECT_FLOAT_EQ(loadedTile->Probability, 0.5f);
    ASSERT_EQ(loadedTile->CollisionPolygons.size(), 1u);
    EXPECT_EQ(loadedTile->CollisionPolygons[0].Points.size(), 3u);
    EXPECT_EQ(loadedTile->CollisionMode, TileCollisionMode::OneWay);
    EXPECT_EQ(loadedTile->PhysicsMaterial, tile->PhysicsMaterial);
    ASSERT_EQ(loadedTile->AnimationFrames.size(), 1u);
    EXPECT_EQ(loadedTile->AnimationFrames[0].TileId, 3u);
    EXPECT_EQ(loadedTile->Peering, tile->Peering);
    EXPECT_EQ(std::get<bool>(loadedTile->CustomData.at("breakable")), true);
    EXPECT_EQ(std::get<std::string>(loadedTile->CustomData.at("name")), "crate");

    ASSERT_EQ(loaded.Brushes().size(), 1u);
    EXPECT_EQ(loaded.Brushes()[0].Name, "Door");
    EXPECT_EQ(loaded.Brushes()[0].Width, 2);
    EXPECT_EQ(loaded.Brushes()[0].Height, 1);
    EXPECT_EQ(loaded.Brushes()[0].Cells, (std::vector<uint32>{5u, 6u}));
}

TEST(TileSetAssetJson, BrushWhoseCellsDoNotMatchItsFootprintIsRejected)
{
    const std::string text = R"({"schemaVersion": 1, "assetType": "TileSet",
        "atlasTexture": "00000000-0000-0000-0000-000000000000", "atlasWidthPx": 32,
        "atlasHeightPx": 32, "tileWidthPx": 16, "tileHeightPx": 16, "marginPx": 0,
        "spacingPx": 0, "columns": 2, "rows": 2, "terrainSets": [], "tiles": [],
        "brushes": [{"name": "Bad", "width": 2, "height": 2, "cells": [1, 2, 3]}]})";
    const Vector<uint8> data(text.begin(), text.end());
    TileSetAsset asset(GUID::Generate(), "bad.tileset");
    EXPECT_FALSE(asset.LoadFromData(data));

    const std::string huge = R"({"schemaVersion": 1, "assetType": "TileSet",
        "atlasTexture": "00000000-0000-0000-0000-000000000000", "atlasWidthPx": 32,
        "atlasHeightPx": 32, "tileWidthPx": 16, "tileHeightPx": 16, "marginPx": 0,
        "spacingPx": 0, "columns": 2, "rows": 2, "terrainSets": [], "tiles": [],
        "brushes": [{"name": "Huge", "width": 100000, "height": 100000, "cells": []}]})";
    const Vector<uint8> hugeData(huge.begin(), huge.end());
    EXPECT_FALSE(asset.LoadFromData(hugeData)) << "a footprint must not size an allocation";
}

TEST(TileSetAssetJson, SerializationIsByteStable)
{
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 64, 64);
    asset.SliceGrid(16, 16, 0, 0);
    asset.FindTile(1)->Probability = 0.75f;

    Vector<uint8> first;
    ASSERT_TRUE(asset.SaveToData(first));

    TileSetAsset loaded(GUID::Generate(), "loaded.tileset");
    ASSERT_TRUE(loaded.LoadFromData(first));
    Vector<uint8> second;
    ASSERT_TRUE(loaded.SaveToData(second));

    EXPECT_EQ(first, second);
}

TEST(TileSetAssetJson, FloatsAreWrittenAtFloatPrecisionAndStillRoundTrip)
{
    // A float32 assigned straight into the JSON is widened to double and dumped
    // in full — 0.04f as "0.03999999910593033". Those digits are the float's
    // representation error, not accuracy, and at four per tile they dominate
    // the file and wreck its diffs.
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 400, 400);
    ASSERT_TRUE(asset.SliceGrid(16, 16, 0, 0));

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));
    const std::string text(data.begin(), data.end());

    EXPECT_EQ(text.find("0.03999999"), std::string::npos)
        << "a UV must not carry the double expansion of a float32";
    EXPECT_NE(text.find("0.04"), std::string::npos) << "0.04f must read as 0.04";

    // Shortening is only legitimate if it is lossless: every tile must come back
    // bit-identical, and a re-save must reproduce the same bytes.
    TileSetAsset loaded(GUID::Generate(), "loaded.tileset");
    ASSERT_TRUE(loaded.LoadFromData(data));
    ASSERT_EQ(loaded.Tiles().size(), asset.Tiles().size());
    for (size_t i = 0; i < asset.Tiles().size(); ++i)
    {
        EXPECT_EQ(loaded.Tiles()[i].UV.U0, asset.Tiles()[i].UV.U0);
        EXPECT_EQ(loaded.Tiles()[i].UV.V0, asset.Tiles()[i].UV.V0);
        EXPECT_EQ(loaded.Tiles()[i].UV.U1, asset.Tiles()[i].UV.U1);
        EXPECT_EQ(loaded.Tiles()[i].UV.V1, asset.Tiles()[i].UV.V1);
    }

    Vector<uint8> second;
    ASSERT_TRUE(loaded.SaveToData(second));
    EXPECT_EQ(data, second);
}

TEST(TileSetAssetJson, DefaultCollisionModeAndMaterialAreOmitted)
{
    // Solid + null material is the default, so a .tileset authored before those
    // fields existed must round-trip byte-identically.
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 64, 64);
    asset.SliceGrid(16, 16, 0, 0);

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());

    EXPECT_EQ(text.find("collisionMode"), std::string::npos);
    EXPECT_EQ(text.find("physicsMaterial"), std::string::npos);
}

TEST(TileSetAssetJson, CollisionModeSerializesAsName)
{
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 64, 64);
    asset.SliceGrid(16, 16, 0, 0);
    asset.FindTile(1)->CollisionMode = TileCollisionMode::OneWay;

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    EXPECT_NE(text.find("\"collisionMode\": \"oneway\""), std::string::npos);

    TileSetAsset loaded(GUID::Generate(), "loaded.tileset");
    ASSERT_TRUE(loaded.LoadFromData(data));
    EXPECT_EQ(loaded.FindTile(1)->CollisionMode, TileCollisionMode::OneWay);
}

TEST(TileSetAssetJson, UnknownCollisionModeFallsBackToSolid)
{
    const std::string text =
        R"({"schemaVersion":1,"assetType":"TileSet","tiles":[{"id":1,"collisionMode":"sideways"}]})";
    Vector<uint8> data(text.begin(), text.end());

    TileSetAsset asset = MakeAsset();
    ASSERT_TRUE(asset.LoadFromData(data));
    ASSERT_NE(asset.FindTile(1), nullptr);
    EXPECT_EQ(asset.FindTile(1)->CollisionMode, TileCollisionMode::Solid);
}

TEST(TileSetAssetJson, MalformedJsonFailsCleanly)
{
    TileSetAsset asset = MakeAsset();
    const std::string garbage = "{ not json";
    Vector<uint8> data(garbage.begin(), garbage.end());
    EXPECT_FALSE(asset.LoadFromData(data));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
    EXPECT_TRUE(asset.Tiles().empty());
}

TEST(TileSetAssetSlice, RefusesAGridAboveTheTileCeiling)
{
    // The product, not either axis, is what sizes the tile table.
    TileSetAsset asset = MakeAsset();
    asset.SetAtlasTexture(GUID::Generate(), 2000000000, 2000000000);
    asset.SliceGrid(1, 1, 0, 0);

    EXPECT_EQ(asset.Columns(), 0);
    EXPECT_EQ(asset.Rows(), 0);
    EXPECT_TRUE(asset.Tiles().empty());
}

TEST(TileSetAssetJson, HostileAtlasGeometryIsRejectedAtLoad)
{
    // Every field a .tileset can lie about, one per case.
    const char* const hostile[] = {
        R"({"atlasWidthPx":2000000000,"atlasHeightPx":2000000000})",
        R"({"atlasWidthPx":-1})",
        R"({"tileWidthPx":-16})",
        R"({"marginPx":-1})",
        R"({"spacingPx":-1})",
        R"({"columns":2000000000,"rows":2000000000})",
        R"({"columns":-4})",
    };
    for (const char* text : hostile)
    {
        TileSetAsset asset = MakeAsset();
        const std::string json(text);
        const Vector<uint8> data(json.begin(), json.end());
        EXPECT_FALSE(asset.LoadFromData(data)) << text;
        EXPECT_EQ(asset.GetState(), AssetState::Failed) << text;
        EXPECT_EQ(asset.Columns(), 0) << text;
        EXPECT_EQ(asset.Rows(), 0) << text;
    }
}

TEST(TileSetAssetJson, WrongFieldTypesFailWithoutThrowing)
{
    // The loader must not depend on exceptions: catch(const std::exception&)
    // is dead in builds whose translation units disagree about RTTI.
    const char* const hostile[] = {
        R"({"atlasWidthPx":"64"})",
        R"({"columns":[1,2]})",
        R"({"tiles":{"id":1}})",
        R"({"tiles":[{"id":"1"}]})",
        R"({"tiles":[{"id":1,"u0":"x"}]})",
        R"({"tiles":[{"id":1,"collision":[[["a","b"],[0,1],[1,1]]]}]})",
        R"({"tiles":[{"id":1,"animation":[{"tileId":"2"}]}]})",
        R"({"terrainSets":[{"name":7}]})",
        R"({"terrainSets":[{"terrains":[{"colorRGBA":"red"}]}]})",
        R"({"tiles":[{"id":1,"peering":{"neighbors":["a","b","c","d","e","f","g","h"]}}]})",
    };
    for (const char* text : hostile)
    {
        TileSetAsset asset = MakeAsset();
        const std::string json(text);
        const Vector<uint8> data(json.begin(), json.end());
        EXPECT_NO_THROW({ (void)asset.LoadFromData(data); }) << text;
    }
}

TEST(TileSetAssetJson, TilesLoadedOutOfOrderStayFindable)
{
    // FindTile binary-searches, so the sort in ParseJson is load-bearing.
    const std::string text =
        R"({"schemaVersion":1,"assetType":"TileSet","tiles":[{"id":9},{"id":2},{"id":5}]})";
    const Vector<uint8> data(text.begin(), text.end());

    TileSetAsset asset = MakeAsset();
    ASSERT_TRUE(asset.LoadFromData(data));
    ASSERT_EQ(asset.Tiles().size(), 3u);
    EXPECT_EQ(asset.Tiles()[0].Id, 2u);
    EXPECT_EQ(asset.Tiles()[1].Id, 5u);
    EXPECT_EQ(asset.Tiles()[2].Id, 9u);
    for (const uint32 id : {2u, 5u, 9u})
        EXPECT_NE(asset.FindTile(id), nullptr) << id;
    EXPECT_EQ(asset.FindTile(3), nullptr);
}

TEST(TileSetAssetJson, TileIdsBeyondTheCellMaskAreRejected)
{
    // A cell can only address kCellTileIdMask; a higher id is unreachable data.
    const std::string text = R"({"schemaVersion":1,"assetType":"TileSet","tiles":[{"id":)" +
                             std::to_string(uint64(kMaxTileId) + 1) + "}]}";
    const Vector<uint8> data(text.begin(), text.end());

    TileSetAsset asset = MakeAsset();
    EXPECT_FALSE(asset.LoadFromData(data));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
    EXPECT_TRUE(asset.Tiles().empty());
}

TEST(TileSetAssetGrid, LargeMarginAndSpacingDoNotWrapGridArithmetic)
{
    constexpr GameEngine::int32 maximum = 2147483647;
    EXPECT_EQ(GameEngine::TileMap::TileSetAsset::GridColumns(32, 16, 1073741824, 0), 0);
    EXPECT_EQ(GameEngine::TileMap::TileSetAsset::GridColumns(32, 16, 0, maximum), 1);
    const auto uv = GameEngine::TileMap::TileSetAsset::ComputeTileUV(32, 32, 16, 16, 0, maximum, 1, 1);
    EXPECT_GT(uv.U0, 0.0f);
    EXPECT_GT(uv.V0, 0.0f);
}
