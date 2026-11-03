#include "TileMapECS/TileSetGpuTables.h"

#include "TileMap/TileSetAsset.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::TileMapECS;

namespace
{

/** Slices `asset` into a 4x2 grid (8 tiles, 16 px cells over a 64x32 atlas). */
void SliceTileSet(TileMap::TileSetAsset& asset)
{
    asset.SetAtlasTexture(GUID::Generate(), 64, 32);
    asset.SliceGrid(16, 16, 0, 0);
}

} // namespace

TEST(TileSetGpuTablesTests, StaticTilesetHasZeroFrameCountsAndDummyFrame)
{
    TileMap::TileSetAsset asset(GUID::Generate(), "tables.tileset");
    SliceTileSet(asset);
    TileSetGpuTables tables;
    ASSERT_TRUE(BuildTileSetGpuTables(asset, tables));
    EXPECT_EQ(tables.MaxTileId, 8u);
    EXPECT_EQ(tables.AnimatedTileCount, 0u);
    ASSERT_EQ(tables.Descs.size(), 8 * kTileDescFloats);
    // Every anim slot zero => the shader's static path for every tile.
    for (size_t tile = 0; tile < 8; ++tile)
        EXPECT_EQ(tables.Descs[tile * kTileDescFloats + 4], 0.0f) << tile;
    // The frames SSBO is never empty, so the binding is unconditional.
    EXPECT_EQ(tables.Frames.size(), kAnimFrameFloats);
    // UV rect of tile 1 (top-left cell).
    EXPECT_FLOAT_EQ(tables.Descs[0], 0.0f);
    EXPECT_FLOAT_EQ(tables.Descs[2], 0.25f);
}

TEST(TileSetGpuTablesTests, AnimatedTileGetsCumulativeFrameTable)
{
    TileMap::TileSetAsset asset(GUID::Generate(), "tables.tileset");
    SliceTileSet(asset);
    TileMap::TileSetTile* tile2 = asset.FindTile(2);
    ASSERT_NE(tile2, nullptr);
    tile2->AnimationFrames = {{2, 0.25f}, {3, 0.5f}, {4, 0.25f}};

    TileSetGpuTables tables;
    ASSERT_TRUE(BuildTileSetGpuTables(asset, tables));
    EXPECT_EQ(tables.AnimatedTileCount, 1u);

    const float32* anim = tables.Descs.data() + 1 * kTileDescFloats + 4;
    EXPECT_FLOAT_EQ(anim[0], 3.0f); // frame count
    EXPECT_FLOAT_EQ(anim[1], 0.0f); // first frame index
    EXPECT_FLOAT_EQ(anim[2], 1.0f); // loop duration

    ASSERT_EQ(tables.Frames.size(), 3 * kAnimFrameFloats);
    EXPECT_FLOAT_EQ(tables.Frames[0], 1.0f);  // tile 2 -> desc index 1
    EXPECT_FLOAT_EQ(tables.Frames[1], 0.25f); // cumulative end times
    EXPECT_FLOAT_EQ(tables.Frames[2], 2.0f);
    EXPECT_FLOAT_EQ(tables.Frames[3], 0.75f);
    EXPECT_FLOAT_EQ(tables.Frames[4], 3.0f);
    EXPECT_FLOAT_EQ(tables.Frames[5], 1.0f);
}

TEST(TileSetGpuTablesTests, InvalidFramesDropAndZeroDurationStaysStatic)
{
    TileMap::TileSetAsset asset(GUID::Generate(), "tables.tileset");
    SliceTileSet(asset);
    // Frame referencing a tile outside the set is dropped; the rest plays.
    asset.FindTile(1)->AnimationFrames = {{99, 0.5f}, {2, 0.5f}};
    // All-zero durations cannot loop; the tile stays static.
    asset.FindTile(3)->AnimationFrames = {{4, 0.0f}, {5, 0.0f}};

    TileSetGpuTables tables;
    ASSERT_TRUE(BuildTileSetGpuTables(asset, tables));
    EXPECT_EQ(tables.AnimatedTileCount, 1u);

    const float32* anim1 = tables.Descs.data() + 0 * kTileDescFloats + 4;
    EXPECT_FLOAT_EQ(anim1[0], 1.0f);
    EXPECT_FLOAT_EQ(anim1[2], 0.5f);

    const float32* anim3 = tables.Descs.data() + 2 * kTileDescFloats + 4;
    EXPECT_FLOAT_EQ(anim3[0], 0.0f);

    // Only tile 1's surviving frame is in the table.
    ASSERT_EQ(tables.Frames.size(), 1 * kAnimFrameFloats);
    EXPECT_FLOAT_EQ(tables.Frames[0], 1.0f);
}

TEST(TileSetGpuTablesTests, EmptyTilesetFails)
{
    TileMap::TileSetAsset asset(GUID::Generate(), "empty.tileset");
    TileSetGpuTables tables;
    EXPECT_FALSE(BuildTileSetGpuTables(asset, tables));
}
