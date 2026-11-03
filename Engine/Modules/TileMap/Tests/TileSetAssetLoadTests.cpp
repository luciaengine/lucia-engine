#include "TileMap/TileSetAsset.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

bool Parse(TileSetAsset& asset, const std::string& text)
{
    const Vector<uint8> bytes(text.begin(), text.end());
    return asset.LoadFromData(bytes);
}

std::string SetJson(const std::string& tiles)
{
    return R"({"atlasTexture": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "tileWidthPx": 16, "tileHeightPx": 8, "tiles": [)" +
           tiles + "]}";
}

} // namespace

TEST(TileSetAssetLoad, LoadsAtlasTileSizeUvsAndAnimation)
{
    TileSetAsset asset(GUID(), "test://set.tileset");
    ASSERT_TRUE(Parse(asset, SetJson(R"({"id": 2, "u0": 0.5, "v0": 0, "u1": 1, "v1": 0.5,
                                         "animation": [{"tileId": 1, "durationSeconds": 0.2},
                                                       {"tileId": 2, "durationSeconds": 0.3}]},
                                        {"id": 1, "u0": 0, "v0": 0, "u1": 0.5, "v1": 0.5})")));

    EXPECT_EQ(asset.AtlasTexture(), GUID("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    EXPECT_EQ(asset.TileWidthPx(), 16);
    EXPECT_EQ(asset.TileHeightPx(), 8);

    // Tiles come back sorted by id whatever order the file lists them in.
    const std::vector<TileSetTile>& tiles = asset.Tiles();
    ASSERT_EQ(tiles.size(), 2u);
    EXPECT_EQ(tiles[0].Id, 1u);
    EXPECT_EQ(tiles[1].Id, 2u);
    EXPECT_FLOAT_EQ(tiles[1].UV.U0, 0.5f);
    EXPECT_FLOAT_EQ(tiles[1].UV.V1, 0.5f);
    ASSERT_EQ(tiles[1].AnimationFrames.size(), 2u);
    EXPECT_EQ(tiles[1].AnimationFrames[0].TileId, 1u);
    EXPECT_FLOAT_EQ(tiles[1].AnimationFrames[1].DurationSeconds, 0.3f);
    EXPECT_TRUE(tiles[0].AnimationFrames.empty());
}

TEST(TileSetAssetLoad, AnimationFramesNamingNoTileAreDropped)
{
    TileSetAsset asset(GUID(), "test://set.tileset");
    ASSERT_TRUE(Parse(asset, SetJson(R"({"id": 1, "animation": [{"tileId": 0}, {"tileId": -5},
                                                                 {"tileId": 1, "durationSeconds": -2}]})")));
    ASSERT_EQ(asset.Tiles()[0].AnimationFrames.size(), 1u);
    // A negative duration clamps to zero rather than running the clock backwards.
    EXPECT_FLOAT_EQ(asset.Tiles()[0].AnimationFrames[0].DurationSeconds, 0.0f);
}

TEST(TileSetAssetLoad, TileIdsAboveTheSetCeilingAreRejected)
{
    // The renderer's descriptor table is sized by the highest id; an id up at
    // the cell mask would size a multi-gigabyte allocation from one line of JSON.
    TileSetAsset hugeId(GUID(), "test://set.tileset");
    EXPECT_FALSE(Parse(hugeId, SetJson(R"({"id": 536870911})")));
    EXPECT_TRUE(hugeId.Tiles().empty());

    TileSetAsset atCeiling(GUID(), "test://set.tileset");
    EXPECT_TRUE(Parse(atCeiling, SetJson(R"({"id": 65536})")));
}

TEST(TileSetAssetLoad, DuplicateAndZeroIdsAreRejected)
{
    TileSetAsset duplicate(GUID(), "test://set.tileset");
    EXPECT_FALSE(Parse(duplicate, SetJson(R"({"id": 3}, {"id": 3})")));

    TileSetAsset zero(GUID(), "test://set.tileset");
    EXPECT_FALSE(Parse(zero, SetJson(R"({"id": 0})")));
}

TEST(TileSetAssetLoad, HostileTileSizeIsRejected)
{
    TileSetAsset asset(GUID(), "test://set.tileset");
    EXPECT_FALSE(Parse(asset, R"({"tileWidthPx": 2000000, "tileHeightPx": 8, "tiles": []})"));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
}

TEST(TileSetAssetLoad, WrongTypesAndBrokenJsonFailWithoutThrowing)
{
    for (const char* text : {"", "{\"tiles\": [", "[]", R"({"tiles": {}})", R"({"tiles": [{"id": "1"}]})"})
    {
        TileSetAsset asset(GUID(), "test://set.tileset");
        EXPECT_FALSE(Parse(asset, text)) << text;
    }
}

TEST(TileSetAssetLoad, RejectsNumbersThatCannotReachRendererAndPhysicsAsFiniteFloats)
{
    for (const char* fields : {
        R"("u0":1e100)", R"("probability":1e100)",
        R"("collision":[[[1e100,0],[0,1],[1,0]]])",
        R"("animation":[{"tileId":1,"durationSeconds":1e100}])",
        R"("customData":{"count":18446744073709551615})"})
    {
        TileSetAsset asset(GUID(), "test://numeric.tileset");
        const std::string text = std::string(R"({"tiles":[{"id":1,)") + fields + "}]}";
        const Vector<uint8> bytes(text.begin(), text.end());
        EXPECT_FALSE(asset.LoadFromData(bytes)) << fields;
    }
}

TEST(TileSetAssetLoad, NonIntegerTerrainNeighborUsesUnsetInsteadOfIntegerOverflow)
{
    TileSetAsset asset(GUID(), "test://neighbor.tileset");
    const std::string text = R"({"tiles":[{"id":1,"peering":{"neighbors":[1e100,-1,-1,-1,-1,-1,-1,-1]}}]})";
    const Vector<uint8> bytes(text.begin(), text.end());
    ASSERT_TRUE(asset.LoadFromData(bytes));
    ASSERT_NE(asset.FindTile(1), nullptr);
    EXPECT_EQ(asset.FindTile(1)->Peering.Neighbors[0], -1);
}
