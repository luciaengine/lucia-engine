#include "Thumbnails/TileMapThumbnailComposite.h"

#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{

constexpr int32 kMaxEdgePx = 256;

/**
 * Adds a Tiles layer and returns it. Layer storage reallocates on add, so the
 * reference is only valid until the next AddTilesLayer on the same map.
 */
TileMap::TileMapLayerData& AddTilesLayer(TileMap::TileMapData& map)
{
    const uint32 id = map.AddLayer("Layer", TileMap::TileMapLayerType::Tiles);
    return *map.FindLayer(id);
}

ThumbnailImage MakeSolidAtlas(int32 width, int32 height, uint8 red, uint8 alpha = 255)
{
    ThumbnailImage image;
    image.Width = width;
    image.Height = height;
    image.Pixels.assign(static_cast<size_t>(width) * height * 4u, 0u);
    for (size_t i = 0; i < image.Pixels.size(); i += 4)
    {
        image.Pixels[i + 0] = red;
        image.Pixels[i + 3] = alpha;
    }
    return image;
}

} // namespace

TEST(TileMapThumbnailLayout, AnEmptyMapHasNoLayout)
{
    TileMap::TileMapData map;
    AddTilesLayer(map);
    EXPECT_FALSE(ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx).Valid);
}

TEST(TileMapThumbnailLayout, BoundsAreThePaintedCellsNotTheirChunks)
{
    TileMap::TileMapData map;
    TileMap::TileMapLayerData& layer = AddTilesLayer(map);
    // Two cells inside one 32-cell chunk: framing the chunk would leave the
    // pair as a speck in the corner of a mostly empty thumbnail.
    layer.SetCell(3, 4, TileMap::MakeCell(1));
    layer.SetCell(5, 9, TileMap::MakeCell(2));

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_EQ(layout.MinCellX, 3);
    EXPECT_EQ(layout.MaxCellX, 5);
    EXPECT_EQ(layout.MinCellY, 4);
    EXPECT_EQ(layout.MaxCellY, 9);
    EXPECT_EQ(layout.CellsWide(), 3);
    EXPECT_EQ(layout.CellsHigh(), 6);
}

TEST(TileMapThumbnailLayout, BoundsSpanEveryTilesLayer)
{
    TileMap::TileMapData map;
    AddTilesLayer(map).SetCell(0, 0, TileMap::MakeCell(1));
    AddTilesLayer(map).SetCell(10, 2, TileMap::MakeCell(1));

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_EQ(layout.MaxCellX, 10);
    EXPECT_EQ(layout.MaxCellY, 2);
}

TEST(TileMapThumbnailLayout, NegativeCellCoordinatesAreInsideTheBounds)
{
    TileMap::TileMapData map;
    TileMap::TileMapLayerData& layer = AddTilesLayer(map);
    layer.SetCell(-40, -33, TileMap::MakeCell(1));
    layer.SetCell(-38, -30, TileMap::MakeCell(1));

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_EQ(layout.MinCellX, -40);
    EXPECT_EQ(layout.MinCellY, -33);
    EXPECT_EQ(layout.CellsWide(), 3);
    EXPECT_EQ(layout.CellsHigh(), 4);
}

TEST(TileMapThumbnailLayout, TheLongEdgeIsTheBudgetAndCellsTileItExactly)
{
    TileMap::TileMapData map;
    TileMap::TileMapLayerData& layer = AddTilesLayer(map);
    layer.SetCell(0, 0, TileMap::MakeCell(1));
    layer.SetCell(9, 4, TileMap::MakeCell(1)); // 10 x 5 cells

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_LE(layout.WidthPx, kMaxEdgePx);
    EXPECT_LE(layout.HeightPx, kMaxEdgePx);
    // Integer cells that tile the image exactly: no half-pixel seams.
    EXPECT_EQ(layout.WidthPx, layout.CellsWide() * layout.CellWidthPx);
    EXPECT_EQ(layout.HeightPx, layout.CellsHigh() * layout.CellHeightPx);
    // The long axis is the one that got the budget.
    EXPECT_EQ(layout.CellWidthPx, kMaxEdgePx / 10);
}

TEST(TileMapThumbnailLayout, NonSquareTilesKeepTheirAspect)
{
    TileMap::TileMapData map;
    TileMap::TileMapLayerData& layer = AddTilesLayer(map);
    layer.SetCell(0, 0, TileMap::MakeCell(1));
    layer.SetCell(3, 3, TileMap::MakeCell(1)); // 4 x 4 cells

    // A 2:1 tile must come out twice as wide as it is tall.
    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 2.0f, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_EQ(layout.CellWidthPx, layout.CellHeightPx * 2);
    EXPECT_LE(layout.WidthPx, kMaxEdgePx);
    EXPECT_LE(layout.HeightPx, kMaxEdgePx);
}

TEST(TileMapThumbnailLayout, AMapWiderThanTheBudgetIsCroppedNotOversized)
{
    TileMap::TileMapData map;
    TileMap::TileMapLayerData& layer = AddTilesLayer(map);
    layer.SetCell(0, 0, TileMap::MakeCell(1));
    layer.SetCell(2000, 0, TileMap::MakeCell(1));

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    // Cells bottom out at one pixel, so the span is what has to give.
    EXPECT_EQ(layout.CellWidthPx, 1);
    EXPECT_EQ(layout.CellHeightPx, 1);
    EXPECT_LE(layout.WidthPx, kMaxEdgePx);
    EXPECT_LE(layout.HeightPx, kMaxEdgePx);
    EXPECT_EQ(layout.MinCellX, 0);
    EXPECT_EQ(layout.CellsWide(), kMaxEdgePx);
}

TEST(TileMapThumbnailLayout, AFarOutlierCellCannotBlowUpTheAllocation)
{
    // One stray cell at a large coordinate used to set the image size directly:
    // 100k cells at one pixel each is a multi-gigabyte composite.
    TileMap::TileMapData map;
    TileMap::TileMapLayerData& layer = AddTilesLayer(map);
    layer.SetCell(0, 0, TileMap::MakeCell(1));
    layer.SetCell(4, 4, TileMap::MakeCell(1));
    layer.SetCell(100000, 90000, TileMap::MakeCell(1));

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_LE(layout.WidthPx, kMaxEdgePx);
    EXPECT_LE(layout.HeightPx, kMaxEdgePx);
    // The kept window is the low corner, where the authored cells are.
    EXPECT_EQ(layout.MinCellX, 0);
    EXPECT_EQ(layout.MinCellY, 0);
}

TEST(TileSetThumbnailLayout, AnOversizedGridIsCroppedToTheBudget)
{
    // 4000 columns of 1px tiles cannot shrink any further, so the column count
    // is what the budget has to cap.
    const TileMapThumbnailLayout layout = ComputeTileSetThumbnailLayout(4000, 2, 1, 1, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_LE(layout.WidthPx, kMaxEdgePx);
    EXPECT_LE(layout.HeightPx, kMaxEdgePx);
}

TEST(TileSetThumbnailLayout, TheGridFitsTheBudgetWithoutMagnifying)
{
    // 4x4 grid of 16px tiles: 256/4 = 64 would magnify, so it clamps to 16.
    const TileMapThumbnailLayout layout = ComputeTileSetThumbnailLayout(4, 4, 16, 16, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_EQ(layout.CellWidthPx, 16);
    EXPECT_EQ(layout.CellHeightPx, 16);
    EXPECT_EQ(layout.WidthPx, 64);
    EXPECT_EQ(layout.HeightPx, 64);
}

TEST(TileSetThumbnailLayout, ABigAtlasIsScaledDownToTheBudget)
{
    // 16x16 grid of 32px tiles is 512px square; the budget forces 16px cells.
    const TileMapThumbnailLayout layout = ComputeTileSetThumbnailLayout(16, 16, 32, 32, kMaxEdgePx);
    ASSERT_TRUE(layout.Valid);
    EXPECT_EQ(layout.CellWidthPx, 16);
    EXPECT_EQ(layout.WidthPx, kMaxEdgePx);
    EXPECT_EQ(layout.HeightPx, kMaxEdgePx);
}

TEST(TileSetThumbnailLayout, AnUnslicedTileSetHasNoLayout)
{
    EXPECT_FALSE(ComputeTileSetThumbnailLayout(0, 0, 16, 16, kMaxEdgePx).Valid);
    EXPECT_FALSE(ComputeTileSetThumbnailLayout(4, 4, 0, 16, kMaxEdgePx).Valid);
}

TEST(TileThumbnailBlit, AOneToOneBlitReproducesTheSourceTexels)
{
    ThumbnailImage atlas = MakeSolidAtlas(4, 4, 0);
    // Mark one texel so the sampling can be checked, not just the coverage.
    atlas.Pixels[(1 * 4 + 2) * 4 + 0] = 200;

    ThumbnailImage target;
    target.Width = 2;
    target.Height = 2;
    target.Pixels.assign(2 * 2 * 4, 0u);

    // Bottom-right 2x2 quadrant of the atlas.
    TileMap::TileUVRect uv{0.5f, 0.25f, 1.0f, 0.75f};
    BlitTileNearest(atlas, uv, target, 0, 0, 2, 2);

    EXPECT_EQ(target.Pixels[(0 * 2 + 0) * 4 + 0], 200);
    EXPECT_EQ(target.Pixels[(0 * 2 + 0) * 4 + 3], 255);
    EXPECT_EQ(target.Pixels[(1 * 2 + 1) * 4 + 0], 0);
    EXPECT_EQ(target.Pixels[(1 * 2 + 1) * 4 + 3], 255);
}

TEST(TileThumbnailBlit, EveryOrientationDrawsTheDihedralImageTheSceneRenders)
{
    // Each texel carries its own red value, so an orientation is identifiable
    // as a permutation and not merely as "something got drawn".
    ThumbnailImage atlas = MakeSolidAtlas(2, 2, 0);
    const uint8 kTexel[4] = {10, 20, 30, 40}; // row-major: (0,0) (1,0) (0,1) (1,1)
    for (size_t i = 0; i < 4; ++i)
        atlas.Pixels[i * 4] = kTexel[i];

    auto drawnRedsFor = [&atlas](TileBlitOrientation orientation) {
        ThumbnailImage target = MakeTransparent(2, 2);
        BlitTileNearest(atlas, TileMap::TileUVRect{}, target, 0, 0, 2, 2, orientation);
        EXPECT_EQ(target.Pixels[3], 255) << "the tile must be drawn at all";
        return std::array<uint8, 4>{target.Pixels[0], target.Pixels[4], target.Pixels[8],
                                    target.Pixels[12]};
    };

    // Expectations follow tilemap.vert's mapping of the quad parameter — the
    // transpose bit swaps the axes first, then the flips mirror — evaluated by
    // hand rather than read back out of this file's implementation. Transpose
    // reflects about the ANTI-diagonal here (10 and 40 trade places, 20 and 30
    // stay), because image rows run opposite to the shader's Y-up parameter.
    using Reds = std::array<uint8, 4>;
    constexpr bool kH = true;
    constexpr bool kV = true;
    constexpr bool kT = true;

    EXPECT_EQ(drawnRedsFor({}), (Reds{10, 20, 30, 40}));
    EXPECT_EQ(drawnRedsFor({kH, false, false}), (Reds{20, 10, 40, 30}));
    EXPECT_EQ(drawnRedsFor({false, kV, false}), (Reds{30, 40, 10, 20}));
    EXPECT_EQ(drawnRedsFor({kH, kV, false}), (Reds{40, 30, 20, 10}));
    EXPECT_EQ(drawnRedsFor({false, false, kT}), (Reds{40, 20, 30, 10}));
    EXPECT_EQ(drawnRedsFor({kH, false, kT}), (Reds{30, 10, 40, 20}));
    EXPECT_EQ(drawnRedsFor({false, kV, kT}), (Reds{20, 40, 10, 30}));
    EXPECT_EQ(drawnRedsFor({kH, kV, kT}), (Reds{10, 30, 20, 40}));
}

TEST(TileThumbnailBlit, TheEightOrientationsAreEightDistinctImages)
{
    // The bits compose as a group action: eight orientations must give eight
    // different images, or two of them are quietly collapsing onto each other.
    ThumbnailImage atlas = MakeSolidAtlas(2, 2, 0);
    for (size_t i = 0; i < 4; ++i)
        atlas.Pixels[i * 4] = static_cast<uint8>(10 * (i + 1));

    std::set<std::vector<uint8>> distinct;
    for (int32 bits = 0; bits < 8; ++bits)
    {
        ThumbnailImage target = MakeTransparent(2, 2);
        BlitTileNearest(atlas, TileMap::TileUVRect{}, target, 0, 0, 2, 2,
                        TileBlitOrientation{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0});
        distinct.insert(target.Pixels);
    }
    EXPECT_EQ(distinct.size(), 8u);
}

TEST(TileThumbnailBlit, AnOrientedDownscaleStaysInsideTheTilesOwnAtlasRegion)
{
    // Right half red, left half green: a mirrored downscale that sampled outside
    // its own rect would show the neighbouring tile's colour.
    ThumbnailImage atlas = MakeSolidAtlas(4, 2, 0);
    for (int32 y = 0; y < 2; ++y)
    {
        for (int32 x = 0; x < 4; ++x)
        {
            const size_t index = (static_cast<size_t>(y) * 4 + x) * 4;
            atlas.Pixels[index + (x < 2 ? 1 : 0)] = 255;
        }
    }

    // The left tile of a two-tile atlas, mirrored, drawn at half size.
    ThumbnailImage target = MakeTransparent(1, 2);
    BlitTileNearest(atlas, TileMap::TileUVRect{0.0f, 0.0f, 0.5f, 1.0f}, target, 0, 0, 1, 2,
                    TileBlitOrientation{/*Horizontal=*/true, false, false});

    EXPECT_EQ(target.Pixels[1], 255) << "sampled the left tile";
    EXPECT_EQ(target.Pixels[0], 0) << "did not reach the neighbouring tile";
}

TEST(TileThumbnailBlit, AReversedRectIsDegenerateNotAFlip)
{
    const ThumbnailImage atlas = MakeSolidAtlas(2, 2, 255);
    ThumbnailImage target = MakeTransparent(2, 2);

    // An orientation is a sampling direction; a rect that runs backwards spans nothing.
    BlitTileNearest(atlas, TileMap::TileUVRect{1.0f, 0.0f, 0.0f, 1.0f}, target, 0, 0, 2, 2);
    EXPECT_EQ(target.Pixels[3], 0);
}

TEST(TileThumbnailBlit, FullyTransparentSourceTexelsLeaveTheTargetAlone)
{
    const ThumbnailImage atlas = MakeSolidAtlas(2, 2, 255, /*alpha=*/0);

    ThumbnailImage target;
    target.Width = 2;
    target.Height = 2;
    target.Pixels.assign(2 * 2 * 4, 0u);
    target.Pixels[1] = 77; // a green the upper layer must not erase
    target.Pixels[3] = 255;

    BlitTileNearest(atlas, TileMap::TileUVRect{}, target, 0, 0, 2, 2);
    EXPECT_EQ(target.Pixels[1], 77);
    EXPECT_EQ(target.Pixels[3], 255);
}

TEST(TileThumbnailBlit, DrawingOutsideTheTargetIsClippedNotWrapped)
{
    const ThumbnailImage atlas = MakeSolidAtlas(2, 2, 255);

    ThumbnailImage target;
    target.Width = 2;
    target.Height = 2;
    target.Pixels.assign(2 * 2 * 4, 0u);

    // Entirely off the right edge, and partially off the top.
    BlitTileNearest(atlas, TileMap::TileUVRect{}, target, 5, 0, 2, 2);
    BlitTileNearest(atlas, TileMap::TileUVRect{}, target, 0, -1, 2, 2);

    // Only the row that landed inside was written.
    EXPECT_EQ(target.Pixels[(0 * 2 + 0) * 4 + 3], 255);
    EXPECT_EQ(target.Pixels[(1 * 2 + 0) * 4 + 3], 0);
}

TEST(TileThumbnailBlit, AnEmptySourceOrTargetIsANoOp)
{
    ThumbnailImage empty;
    ThumbnailImage target;
    target.Width = 1;
    target.Height = 1;
    target.Pixels.assign(4, 0u);

    BlitTileNearest(empty, TileMap::TileUVRect{}, target, 0, 0, 1, 1);
    EXPECT_EQ(target.Pixels[3], 0);

    const ThumbnailImage atlas = MakeSolidAtlas(2, 2, 255);
    ThumbnailImage emptyTarget;
    BlitTileNearest(atlas, TileMap::TileUVRect{}, emptyTarget, 0, 0, 1, 1);
    EXPECT_TRUE(emptyTarget.IsEmpty());
}
