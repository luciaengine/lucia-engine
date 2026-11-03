#include "TileMap/TileCellEncoding.h"
#include "TileMapECS/TileChunkInstances.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::TileMapECS;
using GameEngine::TileMap::kChunkSize;
using GameEngine::TileMap::MakeCell;
using GameEngine::TileMap::TileMapChunk;

TEST(TileChunkInstances, EmptyChunkProducesNoInstances)
{
    TileMapChunk chunk{};
    std::vector<TileInstance> instances{TileInstance{7, 7}}; // must be cleared
    BuildChunkInstances(chunk, instances);
    EXPECT_TRUE(instances.empty());
}

TEST(TileChunkInstances, SkipsEmptyCellsAndKeepsLocalIndices)
{
    TileMapChunk chunk{};
    chunk.Cells[0] = MakeCell(1);
    chunk.Cells[5] = MakeCell(2);
    chunk.Cells[kChunkSize * 3 + 4] = MakeCell(3);

    std::vector<TileInstance> instances;
    BuildChunkInstances(chunk, instances);

    ASSERT_EQ(instances.size(), 3u);
    EXPECT_EQ(instances[0].LocalIndex, 0u);
    EXPECT_EQ(instances[1].LocalIndex, 5u);
    EXPECT_EQ(instances[2].LocalIndex, static_cast<uint32>(kChunkSize * 3 + 4));
    // Row-major cell order, which is what the shader's local-index decode assumes.
    EXPECT_LT(instances[0].LocalIndex, instances[1].LocalIndex);
    EXPECT_LT(instances[1].LocalIndex, instances[2].LocalIndex);
}

TEST(TileChunkInstances, PreservesFlipAndTransposeBits)
{
    TileMapChunk chunk{};
    chunk.Cells[0] = MakeCell(11, /*flipH*/ true, /*flipV*/ false, /*transpose*/ false);
    chunk.Cells[1] = MakeCell(12, false, true, false);
    chunk.Cells[2] = MakeCell(13, false, false, true);
    chunk.Cells[3] = MakeCell(14, true, true, true);

    std::vector<TileInstance> instances;
    BuildChunkInstances(chunk, instances);

    ASSERT_EQ(instances.size(), 4u);
    for (size_t i = 0; i < instances.size(); ++i)
        EXPECT_EQ(instances[i].Cell, chunk.Cells[i]) << "instance " << i;

    EXPECT_TRUE(TileMap::CellFlipH(instances[0].Cell));
    EXPECT_TRUE(TileMap::CellFlipV(instances[1].Cell));
    EXPECT_TRUE(TileMap::CellTranspose(instances[2].Cell));
    EXPECT_EQ(TileMap::CellTileId(instances[3].Cell), 14u);
    EXPECT_TRUE(TileMap::CellFlipH(instances[3].Cell));
    EXPECT_TRUE(TileMap::CellFlipV(instances[3].Cell));
    EXPECT_TRUE(TileMap::CellTranspose(instances[3].Cell));
}

TEST(TileChunkInstances, FullChunkInstancesEveryCell)
{
    TileMapChunk chunk{};
    for (int32 i = 0; i < TileMap::kChunkCellCount; ++i)
        chunk.Cells[static_cast<size_t>(i)] = MakeCell(1);

    std::vector<TileInstance> instances;
    BuildChunkInstances(chunk, instances);
    EXPECT_EQ(instances.size(), static_cast<size_t>(TileMap::kChunkCellCount));
}
