#pragma once

#include "AssetCore/Types.h"
#include "TileMap/TileSetAsset.h"
#include "Thumbnails/ThumbnailImageOps.h"

namespace GameEngine
{

namespace TileMap
{
class TileMapData;
}

namespace Editor
{

/**
 * @brief Where a thumbnail's tiles land: the cell rectangle it covers and the
 * pixel box each cell occupies.
 *
 * Cells get an integer pixel size so every tile is the same size and they tile
 * the image exactly — a fractional cell size would leave seams between tiles at
 * some positions and not others, which reads as a broken atlas rather than as
 * a small one.
 */
struct TileMapThumbnailLayout
{
    int32 MinCellX = 0;
    int32 MinCellY = 0;
    int32 MaxCellX = 0;
    int32 MaxCellY = 0;
    int32 CellWidthPx = 0;
    int32 CellHeightPx = 0;
    int32 WidthPx = 0;
    int32 HeightPx = 0;
    bool Valid = false;

    int32 CellsWide() const { return MaxCellX - MinCellX + 1; }
    int32 CellsHigh() const { return MaxCellY - MinCellY + 1; }
};

/**
 * Fits every painted cell of every Tiles layer into an image whose long edge is
 * at most `maxEdgePx`, preserving the aspect ratio of the used region and of a
 * single tile. `tileAspect` is the tileset's tile width / height. Invalid when
 * the map has no painted cell.
 *
 * `onlyLayerId` restricts the fit to one layer, so a per-layer preview frames
 * that layer's own painted cells instead of the whole map's. A sparse decor
 * layer measured against the map would be mostly empty, which is exactly what
 * makes the layers of one level indistinguishable at icon size. 0 means every
 * layer, matching TileMapLayerRef's "0 = unset" convention for a layer id.
 *
 * Cells never shrink below one pixel, so a bounding box wider than `maxEdgePx`
 * cells is CROPPED to its low corner rather than scaled: the budget is a hard
 * cap on the allocation, not a target the fit may overshoot.
 */
TileMapThumbnailLayout ComputeTileMapThumbnailLayout(const TileMap::TileMapData& map,
                                                     float32 tileAspect, int32 maxEdgePx,
                                                     uint32 onlyLayerId = 0);

/**
 * The same fit for a tileset's own grid: `columns` x `rows` tiles of
 * `tileWidthPx` x `tileHeightPx`, never scaled up past their source size —
 * a 16px tile blown up to 64px would only add blur, not detail.
 */
TileMapThumbnailLayout ComputeTileSetThumbnailLayout(int32 columns, int32 rows, int32 tileWidthPx,
                                                     int32 tileHeightPx, int32 maxEdgePx);

/**
 * @brief One of the eight orientations a cell's flip/transpose bits encode.
 *
 * An orientation is a sampling direction, never a reversed `TileUVRect`: a rect
 * whose U1 precedes its U0 spans a negative width, which this file and
 * `ThumbnailImageOps` both treat as degenerate and decline to draw.
 *
 * The three bits compose in the Tiled GID order `TileCellOrientation` defines —
 * transpose, then horizontal, then vertical — so the fields are not independent
 * mirrors and must be applied in that sequence.
 */
struct TileBlitOrientation
{
    bool Horizontal = false;
    bool Vertical = false;
    bool Transpose = false;
};

/**
 * Draws the atlas region `uv` into the destination rectangle with nearest
 * sampling and source alpha, oriented per `orientation` and clipped to the
 * target. Pixel art must not blur, and a tile drawn over an empty cell must not
 * erase what a lower layer put there — so this composites rather than
 * overwrites.
 */
void BlitTileNearest(const ThumbnailImage& atlas, const TileMap::TileUVRect& uv,
                     ThumbnailImage& target, int32 dstX, int32 dstY, int32 dstWidth,
                     int32 dstHeight, TileBlitOrientation orientation = {});

} // namespace Editor
} // namespace GameEngine
