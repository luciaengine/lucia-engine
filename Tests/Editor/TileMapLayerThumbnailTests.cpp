#include "Thumbnails/TileMapThumbnailComposite.h"

#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{

constexpr int32 kMaxEdgePx = 256;

/**
 * Adds a Tiles layer and returns its id. Layer storage reallocates on add, so
 * the id — not a reference — is what survives adding the next layer.
 */
uint32 AddTilesLayerId(TileMap::TileMapData& map)
{
    return map.AddLayer("Layer", TileMap::TileMapLayerType::Tiles);
}

/**
 * A level's layers: a wide background band, and a small decor cluster off in
 * one corner of it.
 */
void BuildTwoLayerMap(TileMap::TileMapData& map, uint32& outBackgroundId, uint32& outDecorId)
{
    outBackgroundId = AddTilesLayerId(map);
    outDecorId = AddTilesLayerId(map);

    TileMap::TileMapLayerData* background = map.FindLayer(outBackgroundId);
    for (int32 x = 0; x < 20; ++x)
        background->SetCell(x, 0, TileMap::MakeCell(1));

    TileMap::TileMapLayerData* decor = map.FindLayer(outDecorId);
    decor->SetCell(18, 5, TileMap::MakeCell(2));
    decor->SetCell(19, 6, TileMap::MakeCell(2));
}

} // namespace

// The whole point of the per-layer preview: two layers of one map must not
// produce the same framing, or every layer row of a level looks identical.
TEST(TileMapLayerThumbnail, EachLayerFramesItsOwnPaintedCells)
{
    TileMap::TileMapData map;
    uint32 backgroundId = 0;
    uint32 decorId = 0;
    BuildTwoLayerMap(map, backgroundId, decorId);

    const TileMapThumbnailLayout background =
        ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, backgroundId);
    const TileMapThumbnailLayout decor =
        ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, decorId);

    ASSERT_TRUE(background.Valid);
    ASSERT_TRUE(decor.Valid);

    // The decor layer frames its own two cells, not the background's span.
    EXPECT_EQ(decor.CellsWide(), 2);
    EXPECT_EQ(decor.CellsHigh(), 2);
    EXPECT_EQ(decor.MinCellX, 18);
    EXPECT_EQ(decor.MinCellY, 5);

    // Two layers of one map must not land on the same window.
    EXPECT_NE(background.MinCellX, decor.MinCellX);
}

// A level layer spans the level: 20x1 fitted into a square row icon is a
// hairline. The preview shows a readable window of it instead.
TEST(TileMapLayerThumbnail, AWideLayerIsNarrowedTowardsTheIconsAspect)
{
    TileMap::TileMapData map;
    uint32 backgroundId = 0;
    uint32 decorId = 0;
    BuildTwoLayerMap(map, backgroundId, decorId);

    const TileMapThumbnailLayout background =
        ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, backgroundId);

    ASSERT_TRUE(background.Valid);
    EXPECT_LT(background.CellsWide(), 20);
    EXPECT_LE(static_cast<float32>(background.CellsWide()),
              static_cast<float32>(background.CellsHigh()) * 1.5f + 0.5f);
}

// Narrowing keeps the part with the most art in it, not an arbitrary edge.
TEST(TileMapLayerThumbnail, NarrowingKeepsTheBusiestWindow)
{
    TileMap::TileMapData map;
    const uint32 id = AddTilesLayerId(map);
    TileMap::TileMapLayerData* layer = map.FindLayer(id);
    // A long sparse run, then a dense cluster at the far end.
    for (int32 x = 0; x < 40; x += 8)
        layer->SetCell(x, 0, TileMap::MakeCell(1));
    for (int32 x = 40; x < 46; ++x)
        for (int32 y = 0; y < 4; ++y)
            layer->SetCell(x, y, TileMap::MakeCell(2));

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, id);

    ASSERT_TRUE(layout.Valid);
    // The window sits over the cluster, not over the sparse run at x=0.
    EXPECT_GE(layout.MinCellX, 35);
}

// A layer that already reads well in a square icon is left alone.
TEST(TileMapLayerThumbnail, AModerateLayerIsNotNarrowed)
{
    TileMap::TileMapData map;
    const uint32 id = AddTilesLayerId(map);
    TileMap::TileMapLayerData* layer = map.FindLayer(id);
    for (int32 x = 0; x < 6; ++x)
        for (int32 y = 0; y < 5; ++y)
            layer->SetCell(x, y, TileMap::MakeCell(1));

    const TileMapThumbnailLayout layout = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, id);

    ASSERT_TRUE(layout.Valid);
    EXPECT_EQ(layout.CellsWide(), 6);
    EXPECT_EQ(layout.CellsHigh(), 5);
}

// Narrowing is a per-layer framing choice. Asset thumbnails show the whole map
// at grid size, where a wide strip is perfectly legible.
TEST(TileMapLayerThumbnail, TheWholeMapLayoutIsNeverNarrowed)
{
    TileMap::TileMapData map;
    const uint32 id = AddTilesLayerId(map);
    TileMap::TileMapLayerData* layer = map.FindLayer(id);
    for (int32 x = 0; x < 40; ++x)
        layer->SetCell(x, 0, TileMap::MakeCell(1));

    const TileMapThumbnailLayout whole = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);

    ASSERT_TRUE(whole.Valid);
    EXPECT_EQ(whole.CellsWide(), 40);
    EXPECT_EQ(whole.CellsHigh(), 1);
}

// A sparse layer measured against the whole map would be a speck in the corner,
// which is exactly the framing the layer filter exists to avoid.
TEST(TileMapLayerThumbnail, TheUnfilteredLayoutStillSpansEveryLayer)
{
    TileMap::TileMapData map;
    uint32 backgroundId = 0;
    uint32 decorId = 0;
    BuildTwoLayerMap(map, backgroundId, decorId);

    const TileMapThumbnailLayout whole = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);

    ASSERT_TRUE(whole.Valid);
    EXPECT_EQ(whole.CellsWide(), 20);
    EXPECT_EQ(whole.CellsHigh(), 7);
    EXPECT_EQ(whole.MinCellX, 0);
}

// Default-argument behaviour is what every existing caller relies on.
TEST(TileMapLayerThumbnail, LayerZeroMeansEveryLayer)
{
    TileMap::TileMapData map;
    uint32 backgroundId = 0;
    uint32 decorId = 0;
    BuildTwoLayerMap(map, backgroundId, decorId);

    const TileMapThumbnailLayout implicit = ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx);
    const TileMapThumbnailLayout explicitZero =
        ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, 0);

    EXPECT_EQ(implicit.CellsWide(), explicitZero.CellsWide());
    EXPECT_EQ(implicit.CellsHigh(), explicitZero.CellsHigh());
    EXPECT_EQ(implicit.MinCellX, explicitZero.MinCellX);
    EXPECT_EQ(implicit.MinCellY, explicitZero.MinCellY);
}

// An empty layer must report "no preview" so the row keeps its generic icon
// instead of showing a blank box.
TEST(TileMapLayerThumbnail, AnEmptyLayerHasNoLayout)
{
    TileMap::TileMapData map;
    const uint32 paintedId = AddTilesLayerId(map);
    const uint32 emptyId = AddTilesLayerId(map);
    map.FindLayer(paintedId)->SetCell(0, 0, TileMap::MakeCell(1));

    EXPECT_TRUE(ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, paintedId).Valid);
    EXPECT_FALSE(ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, emptyId).Valid);
}

// An IntGrid layer (the shape a Collision layer takes) carries no tiles, so it
// has nothing to draw even when its cells hold values.
TEST(TileMapLayerThumbnail, AnIntGridLayerHasNoLayout)
{
    TileMap::TileMapData map;
    const uint32 intGridId = map.AddLayer("Collision", TileMap::TileMapLayerType::IntGrid);
    map.FindLayer(intGridId)->SetIntGridValue(2, 3, 1);

    EXPECT_FALSE(ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, intGridId).Valid);
}

// An id no layer carries frames nothing, rather than silently falling back to
// the whole map — a stale LayerId on an entity must not draw a sibling's art.
TEST(TileMapLayerThumbnail, AnUnknownLayerIdHasNoLayout)
{
    TileMap::TileMapData map;
    const uint32 paintedId = AddTilesLayerId(map);
    map.FindLayer(paintedId)->SetCell(0, 0, TileMap::MakeCell(1));

    EXPECT_FALSE(ComputeTileMapThumbnailLayout(map, 1.0f, kMaxEdgePx, paintedId + 99).Valid);
}
