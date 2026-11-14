#include "Panels/TileSetCollisionEditor.h"

#include "TileMap/CollisionEdgeMerge.h"
#include "TileMap/TileSetAsset.h"
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

// Canvas metrics, in logical CSS px.
constexpr float32 kPaddingPx = 6.0f;
constexpr float32 kMinCellPx = 24.0f;
constexpr float32 kMaxCellPx = 160.0f;
constexpr float32 kCornerRadiusPx = 4.0f;
constexpr float32 kZoomStep = 1.15f;
constexpr float32 kMinZoom = 1.0f;
constexpr float32 kMaxZoom = 8.0f;
constexpr float32 kOutlineWidthPx = 1.5f;
constexpr float32 kDraftMarkerPx = 4.0f;

// Custom outline points snap to this many steps per tile edge, so hand-placed
// slopes land on clean fractions and opposing tiles butt together exactly.
constexpr int32 kSnapStepsPerTile = 8;

// Pointer buttons as delivered in UIEvent::Button.
constexpr int32 kMouseButtonLeft = 0;
constexpr int32 kMouseButtonRight = 1;
constexpr int32 kMouseButtonMiddle = 2;

// UIPrimitive packs colour as r | g<<8 | b<<16 | a<<24 — always build these
// componentwise.
const uint32 kBackgroundColor = UI::PackColorU8(0x16, 0x18, 0x1A, 0xFF);
const uint32 kMissingTileColor = UI::PackColorU8(0x2E, 0x32, 0x36, 0xFF);
const uint32 kTileTint = UI::PackColorU8(0xFF, 0xFF, 0xFF, 0xFF);
const uint32 kCollisionFillColor = UI::PackColorU8(0x2F, 0xA8, 0x5C, 0x60);
const uint32 kCollisionEdgeColor = UI::PackColorU8(0x3F, 0xE0, 0x74, 0xFF);
const uint32 kDraftPointColor = UI::PackColorU8(0xFF, 0xC8, 0x3A, 0xFF);
const uint32 kSelectionOutlineColor = UI::PackColorU8(0x4F, 0xA3, 0xFF, 0xFF);
const uint32 kOneWayMarkerColor = UI::PackColorU8(0xFF, 0x8A, 0x2B, 0xFF);

// One-way marker: a bar on the tile's top edge with an up-chevron under it —
// the edge that blocks a landing, and the direction a body passes through.
constexpr float32 kOneWayBarFraction = 0.10f;
constexpr float32 kMinOneWayBarPx = 2.0f;
constexpr float32 kMaxOneWayBarPx = 4.0f;
constexpr float32 kOneWayChevronFraction = 0.18f;

} // namespace

TileSetCollisionEditor::TileSetCollisionEditor()
{
    SetId("TileSetCollisionCanvas");
    AddClass("ts-collision-canvas");
}

void TileSetCollisionEditor::SetTileSet(TileMap::TileSetAsset* tileSet)
{
    m_TileSet = tileSet;
    m_Zoom = kMinZoom;
    m_PanX = 0.0f;
    m_PanY = 0.0f;
    m_DraftColumn = -1;
    m_DraftRow = -1;
    m_DraftPoints.clear();
    m_SelectedColumn = -1;
    m_SelectedRow = -1;
    Refresh();
}

void TileSetCollisionEditor::Refresh()
{
    const int32 previousColumn = m_SelectedColumn;
    const int32 previousRow = m_SelectedRow;

    if (m_TileSet == nullptr)
    {
        m_AtlasTexture = GUID();
        m_Columns = 0;
        m_Rows = 0;
        m_TileAspect = 1.0f;
    }
    else
    {
        m_AtlasTexture = m_TileSet->AtlasTexture();
        m_Columns = m_TileSet->Columns();
        m_Rows = m_TileSet->Rows();
        const float32 tileHeight = static_cast<float32>(std::max(1, m_TileSet->TileHeightPx()));
        m_TileAspect = static_cast<float32>(std::max(1, m_TileSet->TileWidthPx())) / tileHeight;
    }

    // A re-slice can shrink the grid out from under the selection.
    if (m_SelectedColumn >= m_Columns || m_SelectedRow >= m_Rows)
    {
        m_SelectedColumn = -1;
        m_SelectedRow = -1;
    }

    MarkDirty(VisualDirty);
    if ((m_SelectedColumn != previousColumn || m_SelectedRow != previousRow) &&
        m_OnSelectionChanged)
    {
        m_OnSelectionChanged();
    }
}

void TileSetCollisionEditor::SetOnCollisionChanged(std::function<void()> callback)
{
    m_OnCollisionChanged = std::move(callback);
}

void TileSetCollisionEditor::SetOnSelectionChanged(std::function<void()> callback)
{
    m_OnSelectionChanged = std::move(callback);
}

uint32 TileSetCollisionEditor::SelectedTileId() const
{
    if (m_SelectedColumn < 0 || m_SelectedRow < 0)
        return 0;
    return TileIdAt(m_SelectedColumn, m_SelectedRow);
}

void TileSetCollisionEditor::SelectTile(int32 column, int32 row)
{
    if (column == m_SelectedColumn && row == m_SelectedRow)
        return;
    m_SelectedColumn = column;
    m_SelectedRow = row;
    MarkDirty(VisualDirty);
    if (m_OnSelectionChanged)
        m_OnSelectionChanged();
}

uint32 TileSetCollisionEditor::TileIdAt(int32 column, int32 row) const
{
    // Grid slices assign ids row-major and 1-based.
    return static_cast<uint32>(row * m_Columns + column) + 1u;
}

TileSetCollisionEditor::Geometry TileSetCollisionEditor::ComputeGeometry(float32 x, float32 y,
                                                                         float32 w,
                                                                         float32 h) const
{
    Geometry geometry;
    if (m_Columns <= 0 || m_Rows <= 0)
        return geometry;

    const float32 availableW = std::max(1.0f, w - 2.0f * kPaddingPx);
    const float32 availableH = std::max(1.0f, h - 2.0f * kPaddingPx);
    const float32 fitW = availableW / static_cast<float32>(m_Columns);
    const float32 fitH = availableH * m_TileAspect / static_cast<float32>(m_Rows);
    const float32 cellW = std::clamp(std::min(fitW, fitH) * m_Zoom, kMinCellPx, kMaxCellPx * m_Zoom);

    geometry.CellW = cellW;
    geometry.CellH = cellW / m_TileAspect;
    geometry.OriginX =
        x + kPaddingPx + m_PanX +
        std::max(0.0f, (availableW - cellW * static_cast<float32>(m_Columns)) * 0.5f);
    geometry.OriginY =
        y + kPaddingPx + m_PanY +
        std::max(0.0f, (availableH - geometry.CellH * static_cast<float32>(m_Rows)) * 0.5f);
    geometry.Valid = true;
    return geometry;
}

bool TileSetCollisionEditor::TileAtPoint(float32 pointX, float32 pointY, int32& outColumn,
                                         int32& outRow, Mathematics::Vector2& outLocal) const
{
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 w = 0.0f;
    float32 h = 0.0f;
    GetHitTestBounds(x, y, w, h);
    const Geometry geometry = ComputeGeometry(x, y, w, h);
    if (!geometry.Valid)
        return false;

    const float32 gridX = (pointX - geometry.OriginX) / geometry.CellW;
    const float32 gridY = (pointY - geometry.OriginY) / geometry.CellH;
    const int32 column = static_cast<int32>(std::floor(gridX));
    const int32 row = static_cast<int32>(std::floor(gridY));
    if (column < 0 || row < 0 || column >= m_Columns || row >= m_Rows)
        return false;

    outColumn = column;
    outRow = row;
    // Screen Y grows downward; collision space is +Y up.
    outLocal.x = std::clamp(gridX - static_cast<float32>(column), 0.0f, 1.0f);
    outLocal.y = 1.0f - std::clamp(gridY - static_cast<float32>(row), 0.0f, 1.0f);
    return true;
}

void TileSetCollisionEditor::ToggleFullSquare(int32 column, int32 row)
{
    if (m_TileSet == nullptr)
        return;
    TileMap::TileSetTile* tile = m_TileSet->FindTile(TileIdAt(column, row));
    if (tile == nullptr)
        return;

    if (column == m_DraftColumn && row == m_DraftRow)
    {
        m_DraftColumn = -1;
        m_DraftRow = -1;
        m_DraftPoints.clear();
    }

    if (tile->CollisionPolygons.empty())
    {
        TileMap::TileCollisionPolygon square;
        square.Points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        tile->CollisionPolygons.push_back(std::move(square));
    }
    else
    {
        tile->CollisionPolygons.clear();
    }

    MarkDirty(VisualDirty);
    if (m_OnCollisionChanged)
        m_OnCollisionChanged();
}

void TileSetCollisionEditor::AddDraftPoint(int32 column, int32 row, Mathematics::Vector2 local)
{
    if (m_TileSet == nullptr)
        return;
    TileMap::TileSetTile* tile = m_TileSet->FindTile(TileIdAt(column, row));
    if (tile == nullptr)
        return;

    if (column != m_DraftColumn || row != m_DraftRow)
    {
        // A draft on a new tile starts that tile's collision from scratch.
        m_DraftColumn = column;
        m_DraftRow = row;
        m_DraftPoints.clear();
        tile->CollisionPolygons.clear();
    }

    const float32 step = 1.0f / static_cast<float32>(kSnapStepsPerTile);
    const Mathematics::Vector2 snapped(std::round(local.x / step) * step,
                                       std::round(local.y / step) * step);
    if (!m_DraftPoints.empty() && std::fabs(m_DraftPoints.back().x - snapped.x) < 1.0e-6f &&
        std::fabs(m_DraftPoints.back().y - snapped.y) < 1.0e-6f)
    {
        return;
    }
    m_DraftPoints.push_back(snapped);
    CommitDraftIfClosed();

    MarkDirty(VisualDirty);
    if (m_OnCollisionChanged)
        m_OnCollisionChanged();
}

void TileSetCollisionEditor::CommitDraftIfClosed()
{
    if (m_DraftPoints.size() < 3 || m_TileSet == nullptr)
        return;
    TileMap::TileSetTile* tile = m_TileSet->FindTile(TileIdAt(m_DraftColumn, m_DraftRow));
    if (tile == nullptr)
        return;
    // The draft is the tile's single outline from the third point on; each
    // further point re-commits it, so the live state always matches the marks.
    TileMap::TileCollisionPolygon polygon;
    polygon.Points = m_DraftPoints;
    tile->CollisionPolygons.clear();
    tile->CollisionPolygons.push_back(std::move(polygon));
}

void TileSetCollisionEditor::ApplyZoom(float32 scrollY, float32 pointerX, float32 pointerY)
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

    const float32 zoom =
        std::clamp(m_Zoom * (scrollY > 0.0f ? kZoomStep : 1.0f / kZoomStep), kMinZoom, kMaxZoom);
    if (zoom == m_Zoom)
        return;
    m_Zoom = zoom;

    // Keep the tile under the pointer under the pointer.
    const Geometry after = ComputeGeometry(x, y, w, h);
    const float32 cellX = (pointerX - before.OriginX) / before.CellW;
    const float32 cellY = (pointerY - before.OriginY) / before.CellH;
    m_PanX += pointerX - (after.OriginX + cellX * after.CellW);
    m_PanY += pointerY - (after.OriginY + cellY * after.CellH);
    MarkDirty(VisualDirty);
}

void TileSetCollisionEditor::OnEvent(UIEvent& e)
{
    if (m_Columns <= 0 || m_Rows <= 0)
        return;

    if (e.Id == kEventScroll)
    {
        ApplyZoom(e.ScrollY, e.X, e.Y);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == kMouseButtonMiddle)
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
        Mathematics::Vector2 local;
        if (TileAtPoint(e.X, e.Y, column, row, local))
        {
            // Select first, act second: a click on a tile the author is only
            // inspecting must not clear the collision they wanted to look at.
            if (column == m_SelectedColumn && row == m_SelectedRow)
                ToggleFullSquare(column, row);
            else
                SelectTile(column, row);
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == kMouseButtonRight)
    {
        int32 column = 0;
        int32 row = 0;
        Mathematics::Vector2 local;
        if (TileAtPoint(e.X, e.Y, column, row, local))
        {
            SelectTile(column, row);
            AddDraftPoint(column, row, local);
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove && m_Panning)
    {
        m_PanX += e.X - m_PanAnchorX;
        m_PanY += e.Y - m_PanAnchorY;
        m_PanAnchorX = e.X;
        m_PanAnchorY = e.Y;
        MarkDirty(VisualDirty);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseUp)
    {
        m_Panning = false;
        e.Stop();
    }
}

void TileSetCollisionEditor::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                  const ResolvedStyle&, float x, float y, float w,
                                                  float h)
{
    // Parallel-drain thread contract: texture resolution touches shared
    // UIManager cache state, so this never runs on a JobSystem worker.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    // UIManager passes physical px; pointer events and the layout rect are in
    // logical CSS px. Everything below is logical and scales at emit time.
    const float32 cs = std::max(0.01f, ctx.ContentScale);
    const float32 logicalX = x / cs;
    const float32 logicalY = y / cs;
    const float32 logicalW = w / cs;
    const float32 logicalH = h / cs;
    if (logicalW <= 1.0f || logicalH <= 1.0f)
        return;

    auto emitRect = [&](float32 rx, float32 ry, float32 rw, float32 rh, uint32 color,
                        float32 radius) {
        if (rw <= 0.0f || rh <= 0.0f)
            return;
        ctx.Emit(UI::MakeRect(rx * cs, ry * cs, rw * cs, rh * cs, color, radius * cs, radius * cs,
                              radius * cs, radius * cs));
    };
    auto emitLine = [&](float32 x0, float32 y0, float32 x1, float32 y1, uint32 color) {
        ctx.Emit(UI::MakeLine(x0 * cs, y0 * cs, x1 * cs, y1 * cs, kOutlineWidthPx * cs, color));
    };

    emitRect(logicalX, logicalY, logicalW, logicalH, kBackgroundColor, kCornerRadiusPx);

    const Geometry geometry = ComputeGeometry(logicalX, logicalY, logicalW, logicalH);
    if (!geometry.Valid || m_TileSet == nullptr)
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
            const TileMap::TileSetTile* tile = m_TileSet->FindTile(TileIdAt(column, row));
            if (tile == nullptr)
                continue;

            const float32 px = geometry.OriginX + static_cast<float32>(column) * geometry.CellW;
            const float32 py = geometry.OriginY + static_cast<float32>(row) * geometry.CellH;
            if (px + geometry.CellW < logicalX || px > logicalX + logicalW ||
                py + geometry.CellH < logicalY || py > logicalY + logicalH)
            {
                continue;
            }

            if (slot != 0)
            {
                UI::UIPrimitive quad =
                    UI::MakeTexturedQuad(px * cs, py * cs, geometry.CellW * cs,
                                         geometry.CellH * cs, slot, kTileTint, tile->UV.U0,
                                         tile->UV.V0, tile->UV.U1, tile->UV.V1);
                // Pixel art must not blur: nearest sampling in the shader.
                quad.ModeAndFlags |= UI::kPrimPointFilterBit;
                ctx.Emit(quad);
            }
            else
            {
                emitRect(px, py, geometry.CellW, geometry.CellH, kMissingTileColor, 0.0f);
            }

            // Tile-local +Y-up point to canvas px (screen Y grows downward).
            auto toCanvas = [&](const Mathematics::Vector2& p) {
                return Mathematics::Vector2(px + p.x * geometry.CellW,
                                            py + (1.0f - p.y) * geometry.CellH);
            };

            for (const TileMap::TileCollisionPolygon& polygon : tile->CollisionPolygons)
            {
                if (polygon.Points.size() < 3)
                    continue;
                if (TileMap::IsFullSquarePolygon(polygon))
                {
                    emitRect(px, py, geometry.CellW, geometry.CellH, kCollisionFillColor, 0.0f);
                    continue;
                }
                // Fill as a triangle fan (exact for the convex outlines physics
                // supports), then trace the outline.
                const Mathematics::Vector2 first = toCanvas(polygon.Points[0]);
                for (size_t i = 1; i + 1 < polygon.Points.size(); ++i)
                {
                    const Mathematics::Vector2 a = toCanvas(polygon.Points[i]);
                    const Mathematics::Vector2 b = toCanvas(polygon.Points[i + 1]);
                    ctx.Emit(UI::MakeTriangle(first.x * cs, first.y * cs, a.x * cs, a.y * cs,
                                              b.x * cs, b.y * cs, kCollisionFillColor));
                }
                for (size_t i = 0; i < polygon.Points.size(); ++i)
                {
                    const Mathematics::Vector2 a = toCanvas(polygon.Points[i]);
                    const Mathematics::Vector2 b =
                        toCanvas(polygon.Points[(i + 1) % polygon.Points.size()]);
                    emitLine(a.x, a.y, b.x, b.y, kCollisionEdgeColor);
                }
            }

            // A one-way tile is solid only to a contact landing on its top face,
            // so the marker sits on that edge and points the way through.
            if (tile->CollisionMode == TileMap::TileCollisionMode::OneWay &&
                !tile->CollisionPolygons.empty())
            {
                const float32 barH = std::clamp(geometry.CellH * kOneWayBarFraction,
                                                kMinOneWayBarPx, kMaxOneWayBarPx);
                emitRect(px, py, geometry.CellW, barH, kOneWayMarkerColor, 0.0f);

                const float32 chevronHalf =
                    std::min(geometry.CellW, geometry.CellH) * kOneWayChevronFraction;
                const float32 centerX = px + geometry.CellW * 0.5f;
                const float32 apexY = py + barH + chevronHalf * 0.5f;
                const float32 baseY = apexY + chevronHalf;
                emitLine(centerX - chevronHalf, baseY, centerX, apexY, kOneWayMarkerColor);
                emitLine(centerX + chevronHalf, baseY, centerX, apexY, kOneWayMarkerColor);
            }

            if (column == m_SelectedColumn && row == m_SelectedRow)
            {
                // Selection outline overlay only — the grid itself stays flush.
                emitLine(px, py, px + geometry.CellW, py, kSelectionOutlineColor);
                emitLine(px + geometry.CellW, py, px + geometry.CellW, py + geometry.CellH,
                         kSelectionOutlineColor);
                emitLine(px + geometry.CellW, py + geometry.CellH, px, py + geometry.CellH,
                         kSelectionOutlineColor);
                emitLine(px, py + geometry.CellH, px, py, kSelectionOutlineColor);
            }

            if (column == m_DraftColumn && row == m_DraftRow)
            {
                for (const Mathematics::Vector2& p : m_DraftPoints)
                {
                    const Mathematics::Vector2 c = toCanvas(p);
                    emitRect(c.x - kDraftMarkerPx * 0.5f, c.y - kDraftMarkerPx * 0.5f,
                             kDraftMarkerPx, kDraftMarkerPx, kDraftPointColor, 1.0f);
                }
            }
        }
    }
}

} // namespace GameEngine
