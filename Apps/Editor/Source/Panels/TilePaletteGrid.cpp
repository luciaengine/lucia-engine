#include "Panels/TilePaletteGrid.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/TilePaletteSettings.h"
#include "Input/InputSystem.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameEngine
{

namespace
{
// Palette canvas metrics, in logical CSS px.
constexpr float32 kGridPaddingPx = 6.0f;
constexpr float32 kMaxCellPx = 64.0f;
constexpr float32 kMinCellPx = 4.0f;
constexpr float32 kSelectionOutlinePx = 2.0f;
constexpr float32 kGridCornerRadiusPx = 4.0f;
// Wheel zoom is multiplicative so a step feels the same at every scale.
constexpr float32 kZoomStep = 1.15f;
constexpr float32 kMinZoom = 1.0f;
constexpr float32 kMaxZoom = 16.0f;

// UIPrimitive packs colour as r | g<<8 | b<<16 | a<<24, so a hex literal reads
// 0xAABBGGRR — writing one as if it were 0xRRGGBBAA silently swaps red and alpha.
// Always build these componentwise.
/** The atlas background is a user setting; UIPrimitive wants it componentwise. */
uint32 GridBackgroundColor()
{
    const uint32 argb = Editor::TilePaletteSettings::Get().GetAtlasBackgroundArgb();
    return UI::PackColorU8(static_cast<uint8>((argb >> 16) & 0xFFu), static_cast<uint8>((argb >> 8) & 0xFFu),
                           static_cast<uint8>(argb & 0xFFu), static_cast<uint8>((argb >> 24) & 0xFFu));
}
const uint32 kGridLineColor = UI::PackColorU8(0x2E, 0x32, 0x36, 0xFF);
const uint32 kTileTint = UI::PackColorU8(0xFF, 0xFF, 0xFF, 0xFF);
const uint32 kSelectionColor = UI::PackColorU8(0x4C, 0x9A, 0xFF, 0xFF);
// Dimmer than the selection so "where the cursor is" never outshouts
// "what is picked".
const uint32 kHoverOutlineColor = UI::PackColorU8(0x4C, 0x9A, 0xFF, 0x99);

// Pointer buttons as delivered in UIEvent::Button.
constexpr int32 kMouseButtonLeft = 0;
constexpr int32 kMouseButtonRight = 1;
constexpr int32 kMouseButtonMiddle = 2;
} // namespace

TilePaletteGrid::TilePaletteGrid()
{
    AddClass("tp-grid");
}

void TilePaletteGrid::SetTileSet(const GUID& atlasTexture, int32 columns, int32 rows,
                                 int32 tileWidthPx, int32 tileHeightPx, std::vector<Cell> cells)
{
    m_AtlasTexture = atlasTexture;
    m_Columns = columns;
    m_Rows = rows;
    m_TileWidthPx = std::max(1, tileWidthPx);
    m_TileAspect = static_cast<float32>(m_TileWidthPx) /
                   static_cast<float32>(std::max(1, tileHeightPx));
    m_Cells = std::move(cells);
    m_Picked.assign(m_Cells.size(), 0);
    m_Zoom = kMinZoom;
    m_PanX = 0.0f;
    m_PanY = 0.0f;
    ClearSelection();
}

void TilePaletteGrid::ZoomToFit()
{
    m_Zoom = kMinZoom;
    m_PanX = 0.0f;
    m_PanY = 0.0f;
    MarkDirty(VisualDirty);
}

void TilePaletteGrid::ZoomToAtlasPixels()
{
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 w = 0.0f;
    float32 h = 0.0f;
    GetHitTestBounds(x, y, w, h);
    const float32 fit = FitCellWidth(w, h);
    if (fit <= 0.0f)
        return;
    m_Zoom = std::clamp(static_cast<float32>(m_TileWidthPx) / fit, kMinZoom, kMaxZoom);
    m_PanX = 0.0f;
    m_PanY = 0.0f;
    MarkDirty(VisualDirty);
}

float32 TilePaletteGrid::AtlasScale() const
{
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 w = 0.0f;
    float32 h = 0.0f;
    GetHitTestBounds(x, y, w, h);
    const Geometry geometry = ComputeGeometry(x, y, w, h);
    return geometry.Valid ? geometry.CellW / static_cast<float32>(m_TileWidthPx) : 0.0f;
}

void TilePaletteGrid::SetOnSelectionChanged(SelectionCallback callback)
{
    m_OnSelectionChanged = std::move(callback);
}

void TilePaletteGrid::ClearSelection()
{
    m_HasSelection = false;
    m_Dragging = false;
    std::fill(m_Picked.begin(), m_Picked.end(), uint8(0));
    MarkDirty(VisualDirty);
}

bool TilePaletteGrid::IsPicked(int32 column, int32 row) const
{
    if (!m_HasSelection || column < 0 || row < 0 || column >= m_Columns || row >= m_Rows)
        return false;
    const size_t index = static_cast<size_t>(row) * static_cast<size_t>(m_Columns) +
                         static_cast<size_t>(column);
    return index < m_Picked.size() && m_Picked[index] != 0;
}

void TilePaletteGrid::PickRectangle()
{
    std::fill(m_Picked.begin(), m_Picked.end(), uint8(0));
    const int32 column0 = std::min(m_AnchorColumn, m_CursorColumn);
    const int32 row0 = std::min(m_AnchorRow, m_CursorRow);
    const int32 column1 = std::max(m_AnchorColumn, m_CursorColumn);
    const int32 row1 = std::max(m_AnchorRow, m_CursorRow);
    for (int32 row = row0; row <= row1; ++row)
        for (int32 column = column0; column <= column1; ++column)
        {
            const size_t index = static_cast<size_t>(row) * static_cast<size_t>(m_Columns) +
                                 static_cast<size_t>(column);
            if (index < m_Picked.size())
                m_Picked[index] = 1;
        }
}

void TilePaletteGrid::FitBoundsToPicks()
{
    int32 column0 = m_Columns, row0 = m_Rows, column1 = -1, row1 = -1;
    for (int32 row = 0; row < m_Rows; ++row)
        for (int32 column = 0; column < m_Columns; ++column)
        {
            const size_t index = static_cast<size_t>(row) * static_cast<size_t>(m_Columns) +
                                 static_cast<size_t>(column);
            if (index >= m_Picked.size() || m_Picked[index] == 0)
                continue;
            column0 = std::min(column0, column);
            row0 = std::min(row0, row);
            column1 = std::max(column1, column);
            row1 = std::max(row1, row);
        }
    m_HasSelection = column1 >= 0;
    if (!m_HasSelection)
        return;
    m_AnchorColumn = column0;
    m_AnchorRow = row0;
    m_CursorColumn = column1;
    m_CursorRow = row1;
}

void TilePaletteGrid::PublishSelection()
{
    // The stamp is published once per gesture, not per moved cell: every
    // publish bumps the edit-context version that every observer polls.
    if (m_OnSelectionChanged && m_HasSelection)
    {
        m_OnSelectionChanged(std::min(m_AnchorColumn, m_CursorColumn),
                             std::min(m_AnchorRow, m_CursorRow),
                             std::max(m_AnchorColumn, m_CursorColumn),
                             std::max(m_AnchorRow, m_CursorRow));
    }
}

uint32 TilePaletteGrid::TileIdAt(int32 column, int32 row) const
{
    if (column < 0 || row < 0 || column >= m_Columns || row >= m_Rows)
        return 0;
    const size_t index = static_cast<size_t>(row) * static_cast<size_t>(m_Columns) +
                         static_cast<size_t>(column);
    return index < m_Cells.size() ? m_Cells[index].Id : 0;
}

void TilePaletteGrid::OnEvent(UIEvent& e)
{
    if (m_Columns <= 0 || m_Rows <= 0)
        return;

    if (e.Id == kEventScroll)
    {
        // Wheel zooms about the pointer, like the Scene View; Shift+wheel pans.
        // Middle or right drag pans as well, so the atlas never needs a modifier
        // to be moved around.
        const bool panModifier = (e.Mods & Input::kModShift) != 0;
        if (panModifier)
        {
            m_PanX += e.ScrollX;
            m_PanY += e.ScrollY;
            MarkDirty(VisualDirty);
        }
        else
        {
            ApplyZoom(e.ScrollY, e.X, e.Y);
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseDown &&
        (e.Button == kMouseButtonMiddle || e.Button == kMouseButtonRight))
    {
        m_Panning = true;
        m_PanAnchorX = e.X;
        m_PanAnchorY = e.Y;
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == kMouseButtonLeft)
    {
        int32 column = 0;
        int32 row = 0;
        if (!CellAtPoint(e.X, e.Y, column, row))
            return;
        // Ctrl toggles one cell in or out of the group; the stamp's bounds
        // follow the group. A toggle is a whole gesture, so it publishes now.
        const bool toggle = (e.Mods & Input::kModControl) != 0 || (e.Mods & Input::kModSuper) != 0;
        if (toggle)
        {
            const size_t index = static_cast<size_t>(row) * static_cast<size_t>(m_Columns) +
                                 static_cast<size_t>(column);
            if (index < m_Picked.size())
            {
                if (!m_HasSelection)
                    std::fill(m_Picked.begin(), m_Picked.end(), uint8(0));
                m_Picked[index] = m_Picked[index] ? 0 : 1;
            }
            FitBoundsToPicks();
            MarkDirty(VisualDirty);
            if (m_HasSelection)
                PublishSelection();
            else
                ClearSelection();
            e.Stop();
            return;
        }
        m_Dragging = true;
        m_HasSelection = true;
        m_AnchorColumn = column;
        m_AnchorRow = row;
        m_CursorColumn = column;
        m_CursorRow = row;
        MarkDirty(VisualDirty);
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseLeave)
    {
        if (m_HoverColumn != -1 || m_HoverRow != -1)
        {
            m_HoverColumn = -1;
            m_HoverRow = -1;
            MarkDirty(VisualDirty);
        }
        return;
    }

    if (e.Id == kEventMouseMove)
    {
        int32 hoverColumn = -1;
        int32 hoverRow = -1;
        if (!CellAtPoint(e.X, e.Y, hoverColumn, hoverRow))
        {
            hoverColumn = -1;
            hoverRow = -1;
        }
        if (hoverColumn != m_HoverColumn || hoverRow != m_HoverRow)
        {
            m_HoverColumn = hoverColumn;
            m_HoverRow = hoverRow;
            MarkDirty(VisualDirty);
        }

        if (m_Panning)
        {
            m_PanX += e.X - m_PanAnchorX;
            m_PanY += e.Y - m_PanAnchorY;
            m_PanAnchorX = e.X;
            m_PanAnchorY = e.Y;
            MarkDirty(VisualDirty);
            e.Stop();
            return;
        }
        if (m_Dragging)
        {
            int32 column = 0;
            int32 row = 0;
            if (CellAtPoint(e.X, e.Y, column, row) &&
                (column != m_CursorColumn || row != m_CursorRow))
            {
                m_CursorColumn = column;
                m_CursorRow = row;
                MarkDirty(VisualDirty);
            }
            e.Stop();
        }
        return;
    }

    if (e.Id == kEventMouseUp)
    {
        if (m_Panning)
        {
            m_Panning = false;
            e.Stop();
            return;
        }
        if (m_Dragging)
        {
            m_Dragging = false;
            PickRectangle();
            PublishSelection();
            e.Stop();
        }
    }
}

void TilePaletteGrid::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                                           float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    // UIManager passes physical px; pointer events and the layout rect are in
    // logical CSS px. Everything below is logical and scales at emit time, so
    // painting and picking run through one transform.
    const float32 cs = std::max(0.01f, ctx.ContentScale);
    const float32 logicalX = x / cs;
    const float32 logicalY = y / cs;
    const float32 logicalW = w / cs;
    const float32 logicalH = h / cs;
    if (logicalW <= 1.0f || logicalH <= 1.0f)
        return;

    // Every primitive carries the inherited clip. The atlas is zoomable and
    // pannable, so without it a zoomed-in tileset draws its cells straight
    // over the rest of the panel and out past the dock edge.
    auto emitRect = [&](float32 rx, float32 ry, float32 rw, float32 rh, uint32 color,
                        float32 radius) {
        ctx.Emit(UI::MakeRect(rx * cs, ry * cs, rw * cs, rh * cs, color, radius * cs,
                              radius * cs, radius * cs, radius * cs, ctx.ClipIndex));
    };

    // The element's own overflow:hidden is NOT handed to a custom emitter --
    // ctx.ClipIndex is the clip inherited from ancestors, and the engine
    // allocates an element's self-clip only on the text path. A zoomed atlas
    // would therefore paint over the whole panel, so quads are trimmed to the
    // grid box here and their UVs trimmed with them.
    auto emitClippedTile = [&](float32 tx, float32 ty, float32 tw, float32 th, uint32 slotIndex,
                               float32 u0, float32 v0, float32 u1, float32 v1) {
        const float32 left = std::max(tx, logicalX);
        const float32 top = std::max(ty, logicalY);
        const float32 right = std::min(tx + tw, logicalX + logicalW);
        const float32 bottom = std::min(ty + th, logicalY + logicalH);
        if (right <= left || bottom <= top || tw <= 0.0f || th <= 0.0f)
            return;

        const float32 uSpan = u1 - u0;
        const float32 vSpan = v1 - v0;
        const float32 cu0 = u0 + uSpan * ((left - tx) / tw);
        const float32 cu1 = u1 - uSpan * ((tx + tw - right) / tw);
        const float32 cv0 = v0 + vSpan * ((top - ty) / th);
        const float32 cv1 = v1 - vSpan * ((ty + th - bottom) / th);

        UI::UIPrimitive quad = UI::MakeTexturedQuad(left * cs, top * cs, (right - left) * cs,
                                                    (bottom - top) * cs, slotIndex, kTileTint,
                                                    cu0, cv0, cu1, cv1, ctx.ClipIndex);
        // Pixel art must not blur: nearest sampling in the shader, so the
        // result does not depend on which sampler the texture cache handed out.
        quad.ModeAndFlags |= UI::kPrimPointFilterBit;
        ctx.Emit(quad);
    };

    auto emitClippedRect = [&](float32 rx, float32 ry, float32 rw, float32 rh, uint32 color) {
        const float32 left = std::max(rx, logicalX);
        const float32 top = std::max(ry, logicalY);
        const float32 right = std::min(rx + rw, logicalX + logicalW);
        const float32 bottom = std::min(ry + rh, logicalY + logicalH);
        if (right <= left || bottom <= top)
            return;
        emitRect(left, top, right - left, bottom - top, color, 0.0f);
    };

    emitRect(logicalX, logicalY, logicalW, logicalH, GridBackgroundColor(), kGridCornerRadiusPx);

    const Geometry geometry = ComputeGeometry(logicalX, logicalY, logicalW, logicalH);
    if (!geometry.Valid)
        return;

    uint32 slot = 0;
    if (ctx.Manager != nullptr && ctx.Textures != nullptr && !m_AtlasTexture.IsNull())
    {
        const Rendering::TextureHandle texture =
            ctx.Manager->BackgroundImages().TryGetHandle(m_AtlasTexture);
        if (texture.IsValid())
            slot = ctx.Textures->Register(texture, ctx.Manager->GetNearestClampSampler());
    }

    for (int32 row = 0; row < m_Rows; ++row)
    {
        for (int32 column = 0; column < m_Columns; ++column)
        {
            const size_t index = static_cast<size_t>(row) * static_cast<size_t>(m_Columns) +
                                 static_cast<size_t>(column);
            if (index >= m_Cells.size() || m_Cells[index].Id == 0)
                continue;

            const Cell& cell = m_Cells[index];
            const float32 px = geometry.OriginX + static_cast<float32>(column) * geometry.CellW;
            const float32 py = geometry.OriginY + static_cast<float32>(row) * geometry.CellH;
            if (px + geometry.CellW < logicalX || px > logicalX + logicalW ||
                py + geometry.CellH < logicalY || py > logicalY + logicalH)
            {
                continue;
            }

            if (slot != 0)
            {
                emitClippedTile(px, py, geometry.CellW, geometry.CellH, slot, cell.U0, cell.V0,
                                cell.U1, cell.V1);
            }
            else
            {
                // Atlas upload has not completed yet — show the slice so the grid
                // reads as populated instead of empty.
                emitClippedRect(px, py, geometry.CellW, geometry.CellH, kGridLineColor);
            }
        }
    }

    // Hover first: the selection outline draws over it when they coincide.
    if (m_HoverColumn >= 0 && m_HoverRow >= 0)
    {
        const float32 hx = geometry.OriginX + static_cast<float32>(m_HoverColumn) * geometry.CellW;
        const float32 hy = geometry.OriginY + static_cast<float32>(m_HoverRow) * geometry.CellH;
        const float32 ht = kSelectionOutlinePx;
        emitClippedRect(hx, hy, geometry.CellW, ht, kHoverOutlineColor);
        emitClippedRect(hx, hy + geometry.CellH - ht, geometry.CellW, ht, kHoverOutlineColor);
        emitClippedRect(hx, hy, ht, geometry.CellH, kHoverOutlineColor);
        emitClippedRect(hx + geometry.CellW - ht, hy, ht, geometry.CellH, kHoverOutlineColor);
    }

    if (!m_HasSelection)
        return;

    const int32 column0 = std::min(m_AnchorColumn, m_CursorColumn);
    const int32 row0 = std::min(m_AnchorRow, m_CursorRow);
    const int32 column1 = std::max(m_AnchorColumn, m_CursorColumn);
    const int32 row1 = std::max(m_AnchorRow, m_CursorRow);
    // A Ctrl-picked group outlines each of its cells; a rectangle (or a drag
    // still in progress) outlines its bounds.
    bool sparse = false;
    if (!m_Dragging)
    {
        for (int32 row = row0; row <= row1 && !sparse; ++row)
            for (int32 column = column0; column <= column1; ++column)
                if (!IsPicked(column, row))
                {
                    sparse = true;
                    break;
                }
    }
    if (sparse)
    {
        const float32 ct = kSelectionOutlinePx;
        for (int32 row = row0; row <= row1; ++row)
            for (int32 column = column0; column <= column1; ++column)
            {
                if (!IsPicked(column, row))
                    continue;
                const float32 cx = geometry.OriginX + static_cast<float32>(column) * geometry.CellW;
                const float32 cy = geometry.OriginY + static_cast<float32>(row) * geometry.CellH;
                emitClippedRect(cx, cy, geometry.CellW, ct, kSelectionColor);
                emitClippedRect(cx, cy + geometry.CellH - ct, geometry.CellW, ct, kSelectionColor);
                emitClippedRect(cx, cy, ct, geometry.CellH, kSelectionColor);
                emitClippedRect(cx + geometry.CellW - ct, cy, ct, geometry.CellH, kSelectionColor);
            }
        return;
    }
    const float32 sx = geometry.OriginX + static_cast<float32>(column0) * geometry.CellW;
    const float32 sy = geometry.OriginY + static_cast<float32>(row0) * geometry.CellH;
    const float32 sw = static_cast<float32>(column1 - column0 + 1) * geometry.CellW;
    const float32 sh = static_cast<float32>(row1 - row0 + 1) * geometry.CellH;
    const float32 t = kSelectionOutlinePx;
    // Trimmed like the tiles: a selection on a zoomed atlas runs off the box
    // just as readily as the cells it outlines.
    emitClippedRect(sx, sy, sw, t, kSelectionColor);
    emitClippedRect(sx, sy + sh - t, sw, t, kSelectionColor);
    emitClippedRect(sx, sy, t, sh, kSelectionColor);
    emitClippedRect(sx + sw - t, sy, t, sh, kSelectionColor);
}

TilePaletteGrid::Geometry TilePaletteGrid::ComputeGeometry(float32 x, float32 y, float32 w,
                                                            float32 h) const
{
    Geometry geometry;
    if (m_Columns <= 0 || m_Rows <= 0)
        return geometry;

    const float32 availableW = std::max(1.0f, w - 2.0f * kGridPaddingPx);
    const float32 availableH = std::max(1.0f, h - 2.0f * kGridPaddingPx);
    // Fit the whole atlas, then let the wheel zoom past the fit for pixel work.
    const float32 cellW =
        std::clamp(FitCellWidth(w, h) * m_Zoom, kMinCellPx, kMaxCellPx * m_Zoom);

    geometry.CellW = cellW;
    geometry.CellH = cellW / m_TileAspect;
    geometry.OriginX = x + kGridPaddingPx + m_PanX +
                       std::max(0.0f, (availableW - cellW * static_cast<float32>(m_Columns)) *
                                          0.5f);
    geometry.OriginY = y + kGridPaddingPx + m_PanY +
                       std::max(0.0f, (availableH - geometry.CellH *
                                                        static_cast<float32>(m_Rows)) *
                                          0.5f);
    geometry.Valid = true;
    return geometry;
}

float32 TilePaletteGrid::FitCellWidth(float32 w, float32 h) const
{
    if (m_Columns <= 0 || m_Rows <= 0)
        return 0.0f;
    const float32 availableW = std::max(1.0f, w - 2.0f * kGridPaddingPx);
    const float32 availableH = std::max(1.0f, h - 2.0f * kGridPaddingPx);
    const float32 fitW = availableW / static_cast<float32>(m_Columns);
    const float32 fitH = availableH * m_TileAspect / static_cast<float32>(m_Rows);
    return std::min(fitW, fitH);
}

bool TilePaletteGrid::CellAtPoint(float32 pointX, float32 pointY, int32& outColumn,
                                  int32& outRow) const
{
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 w = 0.0f;
    float32 h = 0.0f;
    GetHitTestBounds(x, y, w, h);
    const Geometry geometry = ComputeGeometry(x, y, w, h);
    if (!geometry.Valid)
        return false;

    const int32 column =
        static_cast<int32>(std::floor((pointX - geometry.OriginX) / geometry.CellW));
    const int32 row = static_cast<int32>(std::floor((pointY - geometry.OriginY) / geometry.CellH));
    if (column < 0 || row < 0 || column >= m_Columns || row >= m_Rows)
        return false;

    outColumn = column;
    outRow = row;
    return true;
}

void TilePaletteGrid::ApplyZoom(float32 scrollY, float32 pointerX, float32 pointerY)
{
    if (scrollY == 0.0f)
        return;

    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 w = 0.0f;
    float32 h = 0.0f;
    GetHitTestBounds(x, y, w, h);
    const Geometry before = ComputeGeometry(x, y, w, h);
    if (!before.Valid)
        return;

    // Same wheel convention as the 2D Scene View dolly: a positive delta zooms
    // out, and the reverse-dolly setting flips both views together.
    bool zoomIn = scrollY < 0.0f;
    if (Editor::SceneViewSettings::Get().GetScrollWheelDollyReversed())
        zoomIn = !zoomIn;
    const float32 zoom =
        std::clamp(m_Zoom * (zoomIn ? kZoomStep : 1.0f / kZoomStep), kMinZoom, kMaxZoom);
    if (zoom == m_Zoom)
        return;
    m_Zoom = zoom;

    // Keep the cell under the pointer under the pointer.
    const Geometry after = ComputeGeometry(x, y, w, h);
    const float32 cellX = (pointerX - before.OriginX) / before.CellW;
    const float32 cellY = (pointerY - before.OriginY) / before.CellH;
    m_PanX += pointerX - (after.OriginX + cellX * after.CellW);
    m_PanY += pointerY - (after.OriginY + cellY * after.CellH);
    MarkDirty(VisualDirty);
}

} // namespace GameEngine
