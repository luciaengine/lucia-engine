#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapChunk.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::TileMap;

TEST(TileCellEncoding, EmptyCellIsZero)
{
    EXPECT_EQ(kEmptyCell, 0u);
    EXPECT_TRUE(CellIsEmpty(kEmptyCell));
    EXPECT_TRUE(CellIsEmpty(MakeCell(0, true, true, true)));
    EXPECT_FALSE(CellIsEmpty(MakeCell(1)));
}

TEST(TileCellEncoding, RoundTripsIdAndFlags)
{
    for (const bool flipH : {false, true})
    {
        for (const bool flipV : {false, true})
        {
            for (const bool transpose : {false, true})
            {
                const TileCell cell = MakeCell(12345u, flipH, flipV, transpose);
                EXPECT_EQ(CellTileId(cell), 12345u);
                EXPECT_EQ(CellFlipH(cell), flipH);
                EXPECT_EQ(CellFlipV(cell), flipV);
                EXPECT_EQ(CellTranspose(cell), transpose);
            }
        }
    }
}

TEST(TileCellEncoding, MaxTileIdSurvivesAllFlags)
{
    const TileCell cell = MakeCell(kMaxTileId, true, true, true);
    EXPECT_EQ(CellTileId(cell), kMaxTileId);
    EXPECT_TRUE(CellFlipH(cell));
    EXPECT_TRUE(CellFlipV(cell));
    EXPECT_TRUE(CellTranspose(cell));
}

TEST(TileCellEncoding, OverflowingIdIsMasked)
{
    EXPECT_EQ(CellTileId(MakeCell(kMaxTileId + 1u)), 0u);
}

TEST(ChunkCoordMath, FloorDivisionForNegativeCells)
{
    EXPECT_EQ(ChunkOfCell(0, 0), (ChunkCoord{0, 0}));
    EXPECT_EQ(ChunkOfCell(31, 31), (ChunkCoord{0, 0}));
    EXPECT_EQ(ChunkOfCell(32, 32), (ChunkCoord{1, 1}));
    EXPECT_EQ(ChunkOfCell(-1, -1), (ChunkCoord{-1, -1}));
    EXPECT_EQ(ChunkOfCell(-32, -32), (ChunkCoord{-1, -1}));
    EXPECT_EQ(ChunkOfCell(-33, -33), (ChunkCoord{-2, -2}));
}

TEST(ChunkCoordMath, LocalIndexCoversChunkRowMajor)
{
    EXPECT_EQ(LocalCellIndex(0, 0), 0);
    EXPECT_EQ(LocalCellIndex(31, 0), 31);
    EXPECT_EQ(LocalCellIndex(0, 1), 32);
    EXPECT_EQ(LocalCellIndex(31, 31), kChunkCellCount - 1);
    // Negative cells map into the same local range.
    EXPECT_EQ(LocalCellX(-1), 31);
    EXPECT_EQ(LocalCellY(-32), 0);
}

TEST(ChunkCoordMath, PackUnpackRoundTripsNegatives)
{
    for (const int32 x : {-1000000, -33, -1, 0, 1, 31, 32, 1000000})
    {
        for (const int32 y : {-999999, -32, 0, 5, 777777})
        {
            const ChunkCoord coord{x, y};
            EXPECT_EQ(UnpackChunkCoord(PackChunkCoord(coord)), coord);
        }
    }
}
