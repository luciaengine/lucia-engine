#include "TileMap/TileMapData.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::TileMap;

TEST(TileMapLayerData, GetOnEmptyLayerReturnsEmptyCell)
{
    TileMapLayerData layer;
    EXPECT_EQ(layer.GetCell(0, 0), kEmptyCell);
    EXPECT_EQ(layer.GetCell(-1000, 1000), kEmptyCell);
    EXPECT_FALSE(layer.HasAnyCells());
}

TEST(TileMapLayerData, SetGetAcrossChunkBordersAndNegatives)
{
    TileMapLayerData layer;
    const TileCell a = MakeCell(7);
    const TileCell b = MakeCell(9, true);
    const TileCell c = MakeCell(11, false, true, true);

    layer.SetCell(0, 0, a);
    layer.SetCell(31, 31, b);   // same chunk as (0,0)
    layer.SetCell(32, 0, c);    // next chunk over
    layer.SetCell(-1, -1, a);   // chunk (-1,-1)
    layer.SetCell(-100000, 100000, b);

    EXPECT_EQ(layer.GetCell(0, 0), a);
    EXPECT_EQ(layer.GetCell(31, 31), b);
    EXPECT_EQ(layer.GetCell(32, 0), c);
    EXPECT_EQ(layer.GetCell(-1, -1), a);
    EXPECT_EQ(layer.GetCell(-100000, 100000), b);
    // Neighbors untouched.
    EXPECT_EQ(layer.GetCell(1, 0), kEmptyCell);
    EXPECT_EQ(layer.GetCell(-2, -1), kEmptyCell);
}

TEST(TileMapLayerData, ErasingToEmptyDoesNotCreateChunks)
{
    TileMapLayerData layer;
    layer.SetCell(5, 5, kEmptyCell);
    EXPECT_FALSE(layer.HasAnyCells());
}

TEST(TileMapLayerData, CompactDropsAllEmptyChunks)
{
    TileMapLayerData layer;
    layer.SetCell(0, 0, MakeCell(1));
    layer.SetCell(100, 100, MakeCell(2));
    layer.SetCell(100, 100, kEmptyCell);
    EXPECT_EQ(layer.SortedChunkCoords().size(), 2u);
    layer.CompactEmptyChunks();
    EXPECT_EQ(layer.SortedChunkCoords().size(), 1u);
    EXPECT_EQ(layer.GetCell(0, 0), MakeCell(1));
}

TEST(TileMapLayerData, SortedChunkCoordsAreStable)
{
    TileMapLayerData layer;
    layer.SetCell(64, 0, MakeCell(1));   // chunk (2,0)
    layer.SetCell(0, 64, MakeCell(1));   // chunk (0,2)
    layer.SetCell(-1, 0, MakeCell(1));   // chunk (-1,0)
    layer.SetCell(0, 0, MakeCell(1));    // chunk (0,0)

    const auto coords = layer.SortedChunkCoords();
    ASSERT_EQ(coords.size(), 4u);
    EXPECT_EQ(coords[0], (ChunkCoord{-1, 0}));
    EXPECT_EQ(coords[1], (ChunkCoord{0, 0}));
    EXPECT_EQ(coords[2], (ChunkCoord{2, 0}));
    EXPECT_EQ(coords[3], (ChunkCoord{0, 2}));
}

TEST(TileMapLayerData, IntGridPlaneIsIndependentOfTilePlane)
{
    TileMapLayerData layer;
    layer.SetIntGridValue(3, 3, 42);
    EXPECT_EQ(layer.GetIntGridValue(3, 3), 42);
    EXPECT_EQ(layer.GetCell(3, 3), kEmptyCell);
    layer.SetIntGridValue(3, 3, 0);
    EXPECT_EQ(layer.GetIntGridValue(3, 3), 0);
}

TEST(TileMapData, LayerIdsAreStableAndNeverReused)
{
    TileMapData map;
    EXPECT_EQ(map.AddLayer("Ground", TileMapLayerType::Tiles), 1u);
    EXPECT_EQ(map.AddLayer("Walls", TileMapLayerType::Tiles), 2u);

    EXPECT_TRUE(map.RemoveLayer(1));
    EXPECT_EQ(map.FindLayer(1), nullptr);
    ASSERT_NE(map.FindLayer(2), nullptr);
    EXPECT_EQ(map.FindLayer(2)->Name, "Walls");

    EXPECT_EQ(map.AddLayer("Decor", TileMapLayerType::IntGrid), 3u);
}

TEST(TileMapData, RestoreNextLayerIdNeverGoesBelowExistingIds)
{
    TileMapData map;
    map.AddLayer("A", TileMapLayerType::Tiles);
    map.AddLayer("B", TileMapLayerType::Tiles);
    map.RestoreNextLayerId(1); // stale value must be corrected
    EXPECT_EQ(map.AddLayer("C", TileMapLayerType::Tiles), 3u);
}

TEST(TileMapData, DuplicateLayerCopiesCellsUnderAFreshIdDirectlyAbove)
{
    TileMapData map;
    const uint32 groundId = map.AddLayer("Ground", TileMapLayerType::Tiles);
    const uint32 wallsId = map.AddLayer("Walls", TileMapLayerType::Tiles);

    TileMapLayerData* ground = map.FindLayer(groundId);
    ASSERT_NE(ground, nullptr);
    const TileCell painted = MakeCell(7);
    const TileCell far = MakeCell(9, true);
    ground->SetCell(2, 3, painted);
    ground->SetCell(-40, 90, far); // a second chunk, so the whole map copies

    const uint32 copyId = map.DuplicateLayer(groundId, "Ground Copy");
    ASSERT_NE(copyId, 0u);
    EXPECT_NE(copyId, groundId);
    EXPECT_NE(copyId, wallsId);

    const TileMapLayerData* copy = map.FindLayer(copyId);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->Name, "Ground Copy");
    EXPECT_EQ(copy->GetCell(2, 3), painted);
    EXPECT_EQ(copy->GetCell(-40, 90), far);

    // Directly above the original, not appended at the end.
    ASSERT_EQ(map.Layers().size(), 3u);
    EXPECT_EQ(map.Layers()[0].Id, groundId);
    EXPECT_EQ(map.Layers()[1].Id, copyId);
    EXPECT_EQ(map.Layers()[2].Id, wallsId);

    // A copy, not an alias: editing one must not reach the other.
    map.FindLayer(copyId)->SetCell(2, 3, MakeCell(11));
    EXPECT_EQ(map.FindLayer(groundId)->GetCell(2, 3), painted);
}

TEST(TileMapData, DuplicateLayerRejectsAnUnknownId)
{
    TileMapData map;
    map.AddLayer("Ground", TileMapLayerType::Tiles);
    EXPECT_EQ(map.DuplicateLayer(4242, "Nope"), 0u);
    EXPECT_EQ(map.Layers().size(), 1u);
}
