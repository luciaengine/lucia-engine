#include "TileMap/TileMapEditContext.h"

#include "TileMap/TileCellEncoding.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{

/**
 * A stamp whose cells carry their own 1-based index as the tile id, so a
 * reordering is readable straight off the resulting id sequence.
 *
 *   width 3, height 2  ->  1 2 3
 *                          4 5 6
 */
TileStamp MakeIndexedStamp(int32 width, int32 height)
{
    TileStamp stamp;
    stamp.Width = width;
    stamp.Height = height;
    for (int32 i = 0; i < width * height; ++i)
        stamp.Cells.push_back(TileMap::MakeCell(static_cast<uint32>(i + 1)));
    return stamp;
}

std::vector<uint32> TileIds(const TileStamp& stamp)
{
    std::vector<uint32> ids;
    ids.reserve(stamp.Cells.size());
    for (const TileMap::TileCell cell : stamp.Cells)
        ids.push_back(TileMap::CellTileId(cell));
    return ids;
}

} // namespace

TEST(TileMapEditContextTests, FlipHorizontalReversesEachRowAndMirrorsEveryCell)
{
    TileMapEditContext context;
    context.SetStamp(MakeIndexedStamp(3, 2));
    context.FlipStampHorizontal();

    // Reordering the footprint without mirroring each cell (or vice versa) is
    // the classic half-done flip, so both halves are asserted.
    EXPECT_EQ(TileIds(context.Stamp()), (std::vector<uint32>{3, 2, 1, 6, 5, 4}));
    for (const TileMap::TileCell cell : context.Stamp().Cells)
        EXPECT_TRUE(TileMap::CellFlipH(cell));
}

TEST(TileMapEditContextTests, FlipVerticalReversesRowOrder)
{
    TileMapEditContext context;
    context.SetStamp(MakeIndexedStamp(3, 2));
    context.FlipStampVertical();

    EXPECT_EQ(TileIds(context.Stamp()), (std::vector<uint32>{4, 5, 6, 1, 2, 3}));
    for (const TileMap::TileCell cell : context.Stamp().Cells)
        EXPECT_TRUE(TileMap::CellFlipV(cell));
}

TEST(TileMapEditContextTests, RotateSwapsTheFootprintDimensions)
{
    TileMapEditContext context;
    context.SetStamp(MakeIndexedStamp(3, 2));
    context.RotateStampClockwise();

    EXPECT_EQ(context.Stamp().Width, 2);
    EXPECT_EQ(context.Stamp().Height, 3);
    // Source (column, row) lands at (Height-1-row, column): the left column ends
    // up as the top row, read bottom-to-top.
    EXPECT_EQ(TileIds(context.Stamp()), (std::vector<uint32>{4, 1, 5, 2, 6, 3}));
}

TEST(TileMapEditContextTests, FourRotationsRestoreTheOriginalStamp)
{
    TileMapEditContext context;
    const TileStamp original = MakeIndexedStamp(3, 2);
    context.SetStamp(original);
    for (int32 i = 0; i < 4; ++i)
        context.RotateStampClockwise();

    EXPECT_EQ(context.Stamp().Width, original.Width);
    EXPECT_EQ(context.Stamp().Height, original.Height);
    EXPECT_EQ(context.Stamp().Cells, original.Cells);
}

TEST(TileMapEditContextTests, FlippingTwiceRestoresTheOriginalStamp)
{
    TileMapEditContext context;
    const TileStamp original = MakeIndexedStamp(3, 2);

    context.SetStamp(original);
    context.FlipStampHorizontal();
    context.FlipStampHorizontal();
    EXPECT_EQ(context.Stamp().Cells, original.Cells);

    context.SetStamp(original);
    context.FlipStampVertical();
    context.FlipStampVertical();
    EXPECT_EQ(context.Stamp().Cells, original.Cells);
}

TEST(TileMapEditContextTests, StampAtReadsRowMajorFromTheTopLeft)
{
    const TileStamp stamp = MakeIndexedStamp(3, 2);
    EXPECT_EQ(TileMap::CellTileId(stamp.At(0, 0)), 1u);
    EXPECT_EQ(TileMap::CellTileId(stamp.At(2, 0)), 3u);
    EXPECT_EQ(TileMap::CellTileId(stamp.At(0, 1)), 4u);
    EXPECT_TRUE(TileMap::CellIsEmpty(stamp.At(3, 0)));
    EXPECT_TRUE(TileMap::CellIsEmpty(stamp.At(-1, 0)));
}

TEST(TileMapEditContextTests, ChangingTheTileSetDropsAStampThatIndexedTheOldOne)
{
    TileMapEditContext context;
    context.SetTileSet(GUID("11111111-1111-1111-1111-111111111111"));
    context.SetStamp(MakeIndexedStamp(2, 2));
    ASSERT_FALSE(context.Stamp().IsEmpty());

    context.SetTileSet(GUID("22222222-2222-2222-2222-222222222222"));
    EXPECT_TRUE(context.Stamp().IsEmpty());
}

TEST(TileMapEditContextTests, VersionAdvancesOnAcceptedMutationsOnly)
{
    TileMapEditContext context;
    const uint64 initial = context.Version();

    context.SetTool(TileMapTool::Eraser);
    const uint64 afterChange = context.Version();
    EXPECT_GT(afterChange, initial);

    // Re-setting the same value is not a change, so observers are not woken.
    context.SetTool(TileMapTool::Eraser);
    EXPECT_EQ(context.Version(), afterChange);
}

TEST(TileMapEditContextTests, PaintTargetNeedsAnEntityAMapAndALayer)
{
    TileMapEditContext context;
    EXPECT_FALSE(context.HasTargetLayer());
    EXPECT_FALSE(context.HasPaintTarget());

    const GUID map("33333333-3333-3333-3333-333333333333");
    const ECS::EntityHandle entity{7u, 1u};
    // Layer 0 is the "unset" sentinel, so a map alone is not a target.
    context.SetTarget(entity, map, 0);
    EXPECT_FALSE(context.HasTargetLayer());
    EXPECT_FALSE(context.HasPaintTarget());

    // A layer no entity draws can be listed and given a tileset, but a stroke
    // has no plane to land on.
    context.SetTarget(ECS::EntityHandle{}, map, 1);
    EXPECT_TRUE(context.HasTargetLayer());
    EXPECT_FALSE(context.HasPaintTarget());

    context.SetTarget(entity, map, 1);
    EXPECT_TRUE(context.HasPaintTarget());
    EXPECT_EQ(context.TargetEntity(), entity);

    context.ClearTarget();
    EXPECT_FALSE(context.HasTargetLayer());
    EXPECT_TRUE(context.TargetMap().IsNull());
}

TEST(TileMapEditContextTests, RetargetingTheSameLayerDoesNotWakeObservers)
{
    TileMapEditContext context;
    const GUID map("33333333-3333-3333-3333-333333333333");
    const ECS::EntityHandle entity{7u, 1u};
    context.SetTarget(entity, map, 1);
    const uint64 version = context.Version();
    context.SetTarget(entity, map, 1);
    EXPECT_EQ(context.Version(), version);
    // Another entity drawing the same layer id is a different target.
    context.SetTarget(ECS::EntityHandle{8u, 1u}, map, 1);
    EXPECT_GT(context.Version(), version);
}
