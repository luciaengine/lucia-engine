#include "TileMap/TileCellOrientation.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{
constexpr uint32 kTileId = 42;

// The eight orientations, as the (flipH, flipV, transpose) triples they encode.
struct OrientationCase
{
    bool FlipH;
    bool FlipV;
    bool Transpose;
};

constexpr OrientationCase kAllOrientations[] = {
    {false, false, false}, {true, false, false}, {false, true, false}, {true, true, false},
    {false, false, true},  {true, false, true},  {false, true, true},  {true, true, true},
};
} // namespace

TEST(TileCellOrientationTests, FlippingTwiceReturnsTheOriginalOrientation)
{
    for (const OrientationCase& c : kAllOrientations)
    {
        const TileCell cell = MakeCell(kTileId, c.FlipH, c.FlipV, c.Transpose);
        EXPECT_EQ(FlipCellHorizontal(FlipCellHorizontal(cell)), cell);
        EXPECT_EQ(FlipCellVertical(FlipCellVertical(cell)), cell);
    }
}

TEST(TileCellOrientationTests, FourQuarterTurnsReturnTheOriginalOrientation)
{
    for (const OrientationCase& c : kAllOrientations)
    {
        const TileCell cell = MakeCell(kTileId, c.FlipH, c.FlipV, c.Transpose);
        TileCell rotated = cell;
        for (int32 i = 0; i < 4; ++i)
            rotated = RotateCellClockwise(rotated);
        EXPECT_EQ(rotated, cell);
    }
}

TEST(TileCellOrientationTests, EveryOperationPreservesTheTileId)
{
    const TileCell cell = MakeCell(kTileId);
    EXPECT_EQ(CellTileId(FlipCellHorizontal(cell)), kTileId);
    EXPECT_EQ(CellTileId(FlipCellVertical(cell)), kTileId);
    EXPECT_EQ(CellTileId(RotateCellClockwise(cell)), kTileId);
}

TEST(TileCellOrientationTests, OperationsOnAnEmptyCellStayEmpty)
{
    EXPECT_EQ(FlipCellHorizontal(kEmptyCell), kEmptyCell);
    EXPECT_EQ(FlipCellVertical(kEmptyCell), kEmptyCell);
    EXPECT_EQ(RotateCellClockwise(kEmptyCell), kEmptyCell);
}

TEST(TileCellOrientationTests, FlippingAnUnorientedCellSetsExactlyItsOwnBit)
{
    const TileCell cell = MakeCell(kTileId);
    EXPECT_EQ(FlipCellHorizontal(cell), MakeCell(kTileId, true, false, false));
    EXPECT_EQ(FlipCellVertical(cell), MakeCell(kTileId, false, true, false));
}

// The trap the group composition exists to avoid: the transpose bit applies
// first, so flipping a transposed cell is not the same as toggling that flip's
// bit. Flipping horizontally maps (u,v) -> (v,u) -> (-v,u), whose canonical
// (transpose, then H, then V) form negates the *first* component: H and
// transpose, not V. Toggling bits naively would give the mirrored-wrong tile.
TEST(TileCellOrientationTests, FlippingATransposedCellIsNotABitToggle)
{
    const TileCell transposed = MakeCell(kTileId, false, false, true);
    EXPECT_EQ(FlipCellHorizontal(transposed), MakeCell(kTileId, true, false, true));
    EXPECT_EQ(FlipCellVertical(transposed), MakeCell(kTileId, false, true, true));
}

TEST(TileCellOrientationTests, TwoQuarterTurnsEqualBothFlips)
{
    for (const OrientationCase& c : kAllOrientations)
    {
        const TileCell cell = MakeCell(kTileId, c.FlipH, c.FlipV, c.Transpose);
        EXPECT_EQ(RotateCellClockwise(RotateCellClockwise(cell)),
                  FlipCellVertical(FlipCellHorizontal(cell)));
    }
}

TEST(TileCellOrientationTests, EveryOrientationIsReachableByRotationsAndFlips)
{
    // The eight bit combinations are one orbit, so composing the generators from
    // the identity must enumerate all of them — the check that the encoding and
    // the group operations agree on what "eight orientations" means.
    std::set<TileCell> reached;
    std::vector<TileCell> frontier{MakeCell(kTileId)};
    while (!frontier.empty())
    {
        const TileCell cell = frontier.back();
        frontier.pop_back();
        if (!reached.insert(cell).second)
            continue;
        frontier.push_back(RotateCellClockwise(cell));
        frontier.push_back(FlipCellHorizontal(cell));
        frontier.push_back(FlipCellVertical(cell));
    }
    EXPECT_EQ(reached.size(), 8u);
}
