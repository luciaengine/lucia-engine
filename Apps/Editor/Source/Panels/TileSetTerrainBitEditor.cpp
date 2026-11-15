#include "Panels/TileSetTerrainBitEditor.h"

#include "TileMap/TerrainPeering.h"
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

// The tile's own terrain, drawn in the middle of the nine regions. Distinct
// from the eight peering slots, which index TerrainPeering::Neighbors.
constexpr int32 kCenterRegion = -1;
constexpr int32 kRegionsPerAxis = 3;

// Pointer buttons as delivered in UIEvent::Button.
constexpr int32 kMouseButtonLeft = 0;
constexpr int32 kMouseButtonRight = 1;
constexpr int32 kMouseButtonMiddle = 2;

// UIPrimitive packs colour as r | g<<8 | b<<16 | a<<24, so a hex literal reads
// 0xAABBGGRR — always build these componentwise.
const uint32 kBackgroundColor = UI::PackColorU8(0x16, 0x18, 0x1A, 0xFF);
const uint32 kMissingTileColor = UI::PackColorU8(0x2E, 0x32, 0x36, 0xFF);
const uint32 kUnassignedColor = UI::PackColorU8(0x00, 0x00, 0x00, 0x66);
const uint32 kTileTint = UI::PackColorU8(0xFF, 0xFF, 0xFF, 0xFF);

/** Terrain overlays are translucent so the tile art stays readable underneath. */
constexpr uint8 kTerrainOverlayAlpha = 0xC0;

uint32 TerrainOverlayColor(uint32 colorRGBA)
{
    return UI::PackColorU8(static_cast<uint8>((colorRGBA >> 24) & 0xFFu),
                           static_cast<uint8>((colorRGBA >> 16) & 0xFFu),
                           static_cast<uint8>((colorRGBA >> 8) & 0xFFu), kTerrainOverlayAlpha);
}

/**
 * Peering slot of the 3x3 region (subColumn, subRow), sub-row 0 being the tile's
 * top edge. The middle region is the tile's own terrain.
 */
int32 RegionForSubCell(int32 subColumn, int32 subRow)
{
    using TileMap::PeeringSlot;
    static constexpr int32 kSlots[kRegionsPerAxis][kRegionsPerAxis] = {
        {static_cast<int32>(PeeringSlot::TopLeft), static_cast<int32>(PeeringSlot::Top),
         static_cast<int32>(PeeringSlot::TopRight)},
        {static_cast<int32>(PeeringSlot::Left), kCenterRegion,
         static_cast<int32>(PeeringSlot::Right)},
        {static_cast<int32>(PeeringSlot::BottomLeft), static_cast<int32>(PeeringSlot::Bottom),
         static_cast<int32>(PeeringSlot::BottomRight)}};
    return kSlots[subRow][subColumn];
}

} // namespace

TileSetTerrainBitEditor::TileSetTerrainBitEditor()
{
    SetId("TileSetTerrainCanvas");
    AddClass("ts-terrain-canvas");
}

void TileSetTerrainBitEditor::SetTileSet(TileMap::TileSetAsset* tileSet)
{
    m_TileSet = tileSet;
    m_Zoom = kMinZoom;
    m_PanX = 0.0f;
    m_PanY = 0.0f;
    Refresh();
}

void TileSetTerrainBitEditor::Refresh()
{
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
    MarkDirty(VisualDirty);
}

void TileSetTerrainBitEditor::SetActiveTerrain(int16 terrainSet, int16 terrain)
{
    m_ActiveTerrainSet = terrainSet;
    m_ActiveTerrain = terrain;
    MarkDirty(VisualDirty);
}

void TileSetTerrainBitEditor::SetOnPeeringChanged(std::function<void()> callback)
{
    m_OnPeeringChanged = std::move(callback);
}

TileSetTerrainBitEditor::Geometry TileSetTerrainBitEditor::ComputeGeometry(float32 x, float32 y,
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

bool TileSetTerrainBitEditor::RegionAtPoint(float32 pointX, float32 pointY, int32& outColumn,
                                            int32& outRow, int32& outRegion) const
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

    const int32 subColumn = std::clamp(
        static_cast<int32>((gridX - static_cast<float32>(column)) * kRegionsPerAxis), 0,
        kRegionsPerAxis - 1);
    const int32 subRow = std::clamp(
        static_cast<int32>((gridY - static_cast<float32>(row)) * kRegionsPerAxis), 0,
        kRegionsPerAxis - 1);

    outColumn = column;
    outRow = row;
    outRegion = RegionForSubCell(subColumn, subRow);
    return true;
}

void TileSetTerrainBitEditor::PaintRegion(int32 column, int32 row, int32 region, int16 terrain)
{
    if (m_TileSet == nullptr || m_ActiveTerrainSet < 0)
        return;
    // Grid slices assign ids row-major and 1-based.
    const uint32 tileId = static_cast<uint32>(row * m_Columns + column) + 1u;
    TileMap::TileSetTile* tile = m_TileSet->FindTile(tileId);
    if (tile == nullptr)
        return;

    // Annotating a tile under a different terrain set replaces the old
    // annotation wholesale: peering indices are meaningless across sets.
    if (terrain >= 0 && tile->Peering.TerrainSet != m_ActiveTerrainSet)
    {
        tile->Peering = TileMap::TerrainPeering{};
        tile->Peering.TerrainSet = m_ActiveTerrainSet;
    }
    else if (terrain < 0 && tile->Peering.TerrainSet != m_ActiveTerrainSet)
    {
        return;
    }

    int16& target = region == kCenterRegion
                        ? tile->Peering.Center
                        : tile->Peering.Neighbors[static_cast<size_t>(region)];
    if (target == terrain)
        return;
    target = terrain;

    // A tile with nothing left annotated must stop being a match candidate, or
    // it would answer for every all-empty neighbourhood in the set.
    if (tile->Peering.Center < 0)
    {
        const bool anyNeighbor =
            std::any_of(tile->Peering.Neighbors.begin(), tile->Peering.Neighbors.end(),
                        [](int16 value) { return value >= 0; });
        if (!anyNeighbor)
            tile->Peering.TerrainSet = -1;
    }

    MarkDirty(VisualDirty);
    if (m_OnPeeringChanged)
        m_OnPeeringChanged();
}

void TileSetTerrainBitEditor::ApplyZoom(float32 scrollY, float32 pointerX, float32 pointerY)
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

void TileSetTerrainBitEditor::OnEvent(UIEvent& e)
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

    if (e.Id == kEventMouseDown &&
        (e.Button == kMouseButtonLeft || e.Button == kMouseButtonRight))
    {
        m_PaintingButton = e.Button;
        int32 column = 0;
        int32 row = 0;
        int32 region = 0;
        if (RegionAtPoint(e.X, e.Y, column, row, region))
            PaintRegion(column, row, region, e.Button == kMouseButtonRight ? -1 : m_ActiveTerrain);
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove)
    {
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
        if (m_PaintingButton >= 0)
        {
            int32 column = 0;
            int32 row = 0;
            int32 region = 0;
            if (RegionAtPoint(e.X, e.Y, column, row, region))
            {
                PaintRegion(column, row, region,
                            m_PaintingButton == kMouseButtonRight ? -1 : m_ActiveTerrain);
            }
            e.Stop();
        }
        return;
    }

    if (e.Id == kEventMouseUp)
    {
        m_Panning = false;
        m_PaintingButton = -1;
        e.Stop();
    }
}

void TileSetTerrainBitEditor::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
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
    // logical CSS px. Everything below is logical and scales at emit time, so
    // painting and picking run through one transform.
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

    const std::vector<TileMap::TerrainSetDef>& terrainSets = m_TileSet->TerrainSets();

    for (int32 row = 0; row < m_Rows; ++row)
    {
        for (int32 column = 0; column < m_Columns; ++column)
        {
            const uint32 tileId = static_cast<uint32>(row * m_Columns + column) + 1u;
            const TileMap::TileSetTile* tile = m_TileSet->FindTile(tileId);
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
                // Pixel art must not blur: nearest sampling in the shader, so the
                // result does not depend on which sampler the texture cache handed out.
                quad.ModeAndFlags |= UI::kPrimPointFilterBit;
                ctx.Emit(quad);
            }
            else
            {
                emitRect(px, py, geometry.CellW, geometry.CellH, kMissingTileColor, 0.0f);
            }

            const TileMap::TerrainPeering& peering = tile->Peering;
            const std::vector<TileMap::TerrainDef>* terrains =
                peering.TerrainSet >= 0 &&
                        static_cast<size_t>(peering.TerrainSet) < terrainSets.size()
                    ? &terrainSets[static_cast<size_t>(peering.TerrainSet)].Terrains
                    : nullptr;

            const float32 regionW = geometry.CellW / static_cast<float32>(kRegionsPerAxis);
            const float32 regionH = geometry.CellH / static_cast<float32>(kRegionsPerAxis);
            for (int32 subRow = 0; subRow < kRegionsPerAxis; ++subRow)
            {
                for (int32 subColumn = 0; subColumn < kRegionsPerAxis; ++subColumn)
                {
                    const int32 region = RegionForSubCell(subColumn, subRow);
                    const int16 terrain = region == kCenterRegion
                                              ? peering.Center
                                              : peering.Neighbors[static_cast<size_t>(region)];

                    uint32 color = kUnassignedColor;
                    if (terrain >= 0 && terrains != nullptr &&
                        static_cast<size_t>(terrain) < terrains->size())
                    {
                        color = TerrainOverlayColor((*terrains)[static_cast<size_t>(terrain)].ColorRGBA);
                    }
                    else if (terrain >= 0)
                    {
                        // Annotated against a terrain the set no longer defines.
                        color = kMissingTileColor;
                    }

                    // Regions tile the cell exactly: an inset here would put a
                    // gap between every region and stop the overlay lining up
                    // with the tile art and with the neighbouring tile.
                    emitRect(px + static_cast<float32>(subColumn) * regionW,
                             py + static_cast<float32>(subRow) * regionH, regionW, regionH, color,
                             0.0f);
                }
            }
        }
    }
}

} // namespace GameEngine
