#include "Thumbnails/TileMapThumbnailComposite.h"

#include "TileMap/TileMapChunk.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace Editor
{
namespace
{

/** Cell edge below which the tiles stop being distinguishable at all. */
constexpr int32 kMinCellPx = 1;

/**
 * Cell size that fits `cellsWide` x `cellsHigh` cells of aspect `tileAspect`
 * into a `maxEdgePx` box. Both axes are integers so the cells tile exactly.
 */
void FitCells(int32 cellsWide, int32 cellsHigh, float32 tileAspect, int32 maxEdgePx,
              int32& outCellWidthPx, int32& outCellHeightPx)
{
    const float32 aspect = tileAspect > 0.0f ? tileAspect : 1.0f;
    // Solve for the largest cell height whose resulting image fits both edges,
    // then derive the width from the tile's own aspect.
    const float32 byWidth =
        static_cast<float32>(maxEdgePx) / (static_cast<float32>(cellsWide) * aspect);
    const float32 byHeight = static_cast<float32>(maxEdgePx) / static_cast<float32>(cellsHigh);
    const float32 cellHeight = std::min(byWidth, byHeight);

    outCellHeightPx = std::max(kMinCellPx, static_cast<int32>(std::floor(cellHeight)));
    outCellWidthPx =
        std::max(kMinCellPx, static_cast<int32>(std::floor(cellHeight * aspect)));
}

/**
 * Caps the cell SPAN once the cell size has bottomed out at its 1px floor, so
 * the image cannot exceed the budget on either edge.
 *
 * A bounding box is set by the outermost painted cell, and one cell at a far
 * coordinate — an importer's stray gid, a mis-click, a hand-edited file — makes
 * that box thousands of cells wide. Without this cap the composite allocates and
 * clears an image of that size on a detached thread with no handler. The kept
 * window is the low corner, which is where authored content sits when a single
 * outlier is what stretched the box.
 */
void ClampCellSpanToBudget(TileMapThumbnailLayout& layout, int32 maxEdgePx)
{
    const int32 maxCellsWide = std::max(1, maxEdgePx / layout.CellWidthPx);
    const int32 maxCellsHigh = std::max(1, maxEdgePx / layout.CellHeightPx);
    if (layout.CellsWide() > maxCellsWide)
        layout.MaxCellX = layout.MinCellX + maxCellsWide - 1;
    if (layout.CellsHigh() > maxCellsHigh)
        layout.MaxCellY = layout.MinCellY + maxCellsHigh - 1;
}

/**
 * Widest a per-layer preview may get before it is narrowed. A level layer spans
 * the whole level — 210x17 cells is ordinary — and fitting that into a square
 * row icon leaves a two-pixel hairline that carries no information at all.
 */
constexpr float32 kMaxPreviewAspect = 1.5f;

/** Painted cells per column (or row) of the layer's bounding box. */
void CollectDensity(const TileMap::TileMapLayerData& layer, const TileMapThumbnailLayout& bounds,
                    std::vector<int32>& outColumns, std::vector<int32>& outRows)
{
    outColumns.assign(static_cast<size_t>(bounds.CellsWide()), 0);
    outRows.assign(static_cast<size_t>(bounds.CellsHigh()), 0);
    for (const TileMap::ChunkCoord coord : layer.SortedChunkCoords())
    {
        const TileMap::TileMapChunk* chunk = layer.FindChunk(coord);
        if (chunk == nullptr)
            continue;
        for (int32 y = 0; y < TileMap::kChunkSize; ++y)
        {
            for (int32 x = 0; x < TileMap::kChunkSize; ++x)
            {
                if (TileMap::CellIsEmpty(
                        chunk->Cells[static_cast<size_t>(y) * TileMap::kChunkSize + x]))
                {
                    continue;
                }
                const int32 column = coord.X * TileMap::kChunkSize + x - bounds.MinCellX;
                const int32 row = coord.Y * TileMap::kChunkSize + y - bounds.MinCellY;
                if (column >= 0 && column < static_cast<int32>(outColumns.size()))
                    ++outColumns[static_cast<size_t>(column)];
                if (row >= 0 && row < static_cast<int32>(outRows.size()))
                    ++outRows[static_cast<size_t>(row)];
            }
        }
    }
}

/** Start offset of the `windowSize`-wide window holding the most painted cells. */
int32 BusiestWindowStart(const std::vector<int32>& density, int32 windowSize)
{
    const int32 count = static_cast<int32>(density.size());
    if (windowSize >= count)
        return 0;

    int32 running = 0;
    for (int32 i = 0; i < windowSize; ++i)
        running += density[static_cast<size_t>(i)];

    int32 best = running;
    int32 bestStart = 0;
    for (int32 start = 1; start + windowSize <= count; ++start)
    {
        running -= density[static_cast<size_t>(start - 1)];
        running += density[static_cast<size_t>(start + windowSize - 1)];
        if (running > best)
        {
            best = running;
            bestStart = start;
        }
    }
    return bestStart;
}

/**
 * Crops an extreme per-layer bounding box down to its busiest window.
 *
 * A row icon cannot show 210 cells of level in ~20 square pixels, so it shows
 * the part with the most art in it instead — which also keeps two layers of one
 * level looking like themselves rather than like two identical hairlines.
 */
void NarrowToBusiestWindow(const TileMap::TileMapData& map, uint32 onlyLayerId,
                           TileMapThumbnailLayout& layout)
{
    const TileMap::TileMapLayerData* layer = nullptr;
    for (const TileMap::TileMapLayerData& candidate : map.Layers())
    {
        if (candidate.Id == onlyLayerId)
        {
            layer = &candidate;
            break;
        }
    }
    if (layer == nullptr)
        return;

    const int32 cellsWide = layout.CellsWide();
    const int32 cellsHigh = layout.CellsHigh();
    const float32 wide = static_cast<float32>(cellsWide);
    const float32 high = static_cast<float32>(cellsHigh);
    const bool tooWide = wide > high * kMaxPreviewAspect;
    const bool tooTall = high > wide * kMaxPreviewAspect;
    if (!tooWide && !tooTall)
        return;

    std::vector<int32> columns;
    std::vector<int32> rows;
    CollectDensity(*layer, layout, columns, rows);

    if (tooWide)
    {
        const int32 window =
            std::max(1, std::min(cellsWide, static_cast<int32>(std::lround(high * kMaxPreviewAspect))));
        const int32 start = BusiestWindowStart(columns, window);
        layout.MinCellX += start;
        layout.MaxCellX = layout.MinCellX + window - 1;
    }
    else
    {
        const int32 window =
            std::max(1, std::min(cellsHigh, static_cast<int32>(std::lround(wide * kMaxPreviewAspect))));
        const int32 start = BusiestWindowStart(rows, window);
        layout.MinCellY += start;
        layout.MaxCellY = layout.MinCellY + window - 1;
    }
}

/** A point in a tile's own source rect, normalized and measured from its top-left. */
struct SamplePoint
{
    float32 U = 0.0f;
    float32 V = 0.0f;
};

/**
 * Maps a normalized destination point inside a cell to the source point the
 * cell's orientation draws there.
 *
 * This mirrors `tilemap.vert` — transpose, then horizontal, then vertical, the
 * Tiled GID order `TileCellOrientation` defines — so a thumbnail shows the
 * orientation the scene renders. The shader works in a Y-up quad parameter
 * while image rows run downward, so the V axis is carried into that frame and
 * back out again; applying the bits directly to image-space coordinates would
 * reflect a transposed tile about the wrong diagonal.
 */
SamplePoint OrientedSample(float32 u, float32 v, TileBlitOrientation orientation)
{
    float32 x = u;
    float32 y = 1.0f - v;
    if (orientation.Transpose)
        std::swap(x, y);
    if (orientation.Horizontal)
        x = 1.0f - x;
    if (orientation.Vertical)
        y = 1.0f - y;
    return SamplePoint{x, 1.0f - y};
}

} // namespace

TileMapThumbnailLayout ComputeTileMapThumbnailLayout(const TileMap::TileMapData& map,
                                                     float32 tileAspect, int32 maxEdgePx,
                                                     uint32 onlyLayerId)
{
    TileMapThumbnailLayout layout;
    if (maxEdgePx <= 0)
        return layout;

    // Bounds come from the painted cells, not from the chunks that hold them: a
    // chunk is 32 cells wide, so chunk bounds would frame a five-cell map inside
    // a mostly empty tile. The scan is still bounded by the materialized chunks.
    bool any = false;
    for (const TileMap::TileMapLayerData& layer : map.Layers())
    {
        if (layer.Type != TileMap::TileMapLayerType::Tiles)
            continue;
        if (onlyLayerId != 0 && layer.Id != onlyLayerId)
            continue;
        for (const TileMap::ChunkCoord coord : layer.SortedChunkCoords())
        {
            const TileMap::TileMapChunk* chunk = layer.FindChunk(coord);
            if (chunk == nullptr)
                continue;
            for (int32 y = 0; y < TileMap::kChunkSize; ++y)
            {
                for (int32 x = 0; x < TileMap::kChunkSize; ++x)
                {
                    if (TileMap::CellIsEmpty(
                            chunk->Cells[static_cast<size_t>(y) * TileMap::kChunkSize + x]))
                    {
                        continue;
                    }
                    const int32 cellX = coord.X * TileMap::kChunkSize + x;
                    const int32 cellY = coord.Y * TileMap::kChunkSize + y;
                    if (!any)
                    {
                        layout.MinCellX = layout.MaxCellX = cellX;
                        layout.MinCellY = layout.MaxCellY = cellY;
                        any = true;
                        continue;
                    }
                    layout.MinCellX = std::min(layout.MinCellX, cellX);
                    layout.MinCellY = std::min(layout.MinCellY, cellY);
                    layout.MaxCellX = std::max(layout.MaxCellX, cellX);
                    layout.MaxCellY = std::max(layout.MaxCellY, cellY);
                }
            }
        }
    }
    if (!any)
        return layout;

    if (onlyLayerId != 0)
        NarrowToBusiestWindow(map, onlyLayerId, layout);

    FitCells(layout.CellsWide(), layout.CellsHigh(), tileAspect, maxEdgePx, layout.CellWidthPx,
             layout.CellHeightPx);
    ClampCellSpanToBudget(layout, maxEdgePx);
    layout.WidthPx = layout.CellsWide() * layout.CellWidthPx;
    layout.HeightPx = layout.CellsHigh() * layout.CellHeightPx;
    layout.Valid = true;
    return layout;
}

TileMapThumbnailLayout ComputeTileSetThumbnailLayout(int32 columns, int32 rows, int32 tileWidthPx,
                                                     int32 tileHeightPx, int32 maxEdgePx)
{
    TileMapThumbnailLayout layout;
    if (columns <= 0 || rows <= 0 || tileWidthPx <= 0 || tileHeightPx <= 0 || maxEdgePx <= 0)
        return layout;

    layout.MinCellX = 0;
    layout.MinCellY = 0;
    layout.MaxCellX = columns - 1;
    layout.MaxCellY = rows - 1;

    const float32 tileAspect =
        static_cast<float32>(tileWidthPx) / static_cast<float32>(tileHeightPx);
    FitCells(columns, rows, tileAspect, maxEdgePx, layout.CellWidthPx, layout.CellHeightPx);

    // Never magnify: a 16px tile drawn into 64px adds blur, not detail.
    layout.CellWidthPx = std::min(layout.CellWidthPx, tileWidthPx);
    layout.CellHeightPx = std::min(layout.CellHeightPx, tileHeightPx);

    ClampCellSpanToBudget(layout, maxEdgePx);

    layout.WidthPx = layout.CellsWide() * layout.CellWidthPx;
    layout.HeightPx = layout.CellsHigh() * layout.CellHeightPx;
    layout.Valid = true;
    return layout;
}

void BlitTileNearest(const ThumbnailImage& atlas, const TileMap::TileUVRect& uv,
                     ThumbnailImage& target, int32 dstX, int32 dstY, int32 dstWidth,
                     int32 dstHeight, TileBlitOrientation orientation)
{
    if (atlas.IsEmpty() || target.IsEmpty() || dstWidth <= 0 || dstHeight <= 0)
        return;

    const float32 atlasW = static_cast<float32>(atlas.Width);
    const float32 atlasH = static_cast<float32>(atlas.Height);
    const float32 srcX0 = uv.U0 * atlasW;
    const float32 srcY0 = uv.V0 * atlasH;
    const float32 srcSpanX = (uv.U1 - uv.U0) * atlasW;
    const float32 srcSpanY = (uv.V1 - uv.V0) * atlasH;
    if (srcSpanX <= 0.0f || srcSpanY <= 0.0f)
        return;

    for (int32 y = 0; y < dstHeight; ++y)
    {
        const int32 targetY = dstY + y;
        if (targetY < 0 || targetY >= target.Height)
            continue;
        // Sample the texel centre of the destination pixel, so a 1:1 blit is
        // exact and a downscale picks a representative texel per pixel. An
        // orientation moves that centre within the cell, which keeps it exact.
        const float32 rowCentre = (static_cast<float32>(y) + 0.5f) / static_cast<float32>(dstHeight);

        for (int32 x = 0; x < dstWidth; ++x)
        {
            const int32 targetX = dstX + x;
            if (targetX < 0 || targetX >= target.Width)
                continue;
            const float32 columnCentre =
                (static_cast<float32>(x) + 0.5f) / static_cast<float32>(dstWidth);

            // Transposing makes each source axis depend on both destination
            // axes, so neither can be hoisted out of the column loop.
            const SamplePoint sample = OrientedSample(columnCentre, rowCentre, orientation);
            const int32 srcX =
                std::clamp(static_cast<int32>(srcX0 + sample.U * srcSpanX), 0, atlas.Width - 1);
            const int32 srcY =
                std::clamp(static_cast<int32>(srcY0 + sample.V * srcSpanY), 0, atlas.Height - 1);

            const size_t srcIndex =
                (static_cast<size_t>(srcY) * static_cast<size_t>(atlas.Width) +
                 static_cast<size_t>(srcX)) * 4u;
            const size_t dstIndex =
                (static_cast<size_t>(targetY) * static_cast<size_t>(target.Width) +
                 static_cast<size_t>(targetX)) * 4u;
            if (srcIndex + 3u >= atlas.Pixels.size() || dstIndex + 3u >= target.Pixels.size())
                continue;

            const uint32 srcAlpha = atlas.Pixels[srcIndex + 3u];
            if (srcAlpha == 0u)
                continue;
            if (srcAlpha == 255u)
            {
                std::copy_n(atlas.Pixels.begin() + static_cast<ptrdiff_t>(srcIndex), 4,
                            target.Pixels.begin() + static_cast<ptrdiff_t>(dstIndex));
                continue;
            }

            // Source-over onto a possibly transparent target, in straight alpha.
            const uint32 dstAlpha = target.Pixels[dstIndex + 3u];
            const uint32 outAlpha = srcAlpha + dstAlpha * (255u - srcAlpha) / 255u;
            for (uint32 channel = 0; channel < 3u; ++channel)
            {
                const uint32 src = atlas.Pixels[srcIndex + channel];
                const uint32 dst = target.Pixels[dstIndex + channel];
                const uint32 blended = src * srcAlpha + dst * dstAlpha * (255u - srcAlpha) / 255u;
                target.Pixels[dstIndex + channel] =
                    static_cast<uint8>(outAlpha > 0u ? blended / outAlpha : 0u);
            }
            target.Pixels[dstIndex + 3u] = static_cast<uint8>(outAlpha);
        }
    }
}

} // namespace Editor
} // namespace GameEngine
