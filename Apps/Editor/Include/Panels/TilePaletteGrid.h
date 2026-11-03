#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "UI/UIElement.h"

#include <functional>
#include <vector>

namespace GameEngine
{

/**
 * @brief The tileset atlas drawn as a grid of pickable tiles.
 *
 * Owns only display state: the panel hands it a flat cell table (row-major from
 * the top-left, exactly how TileStamp is laid out) and reads the selection
 * rectangle back out. Wheel zooms about the pointer, Shift+wheel pans,
 * middle/right drag pans, and a left drag selects a rectangle of cells that
 * is published once on release. Ctrl-click toggles one cell in or out of the
 * selection, so a stamp can be a sparse group; the published rectangle is the
 * group's bounds and IsPicked says which cells inside it are in.
 */
class TilePaletteGrid : public UIElement
{
  public:
    /** One grid cell. Id 0 means the slice produced no tile there. */
    struct Cell
    {
        uint32 Id = 0;
        float32 U0 = 0.0f;
        float32 V0 = 0.0f;
        float32 U1 = 0.0f;
        float32 V1 = 0.0f;
    };

    using SelectionCallback =
        std::function<void(int32 column0, int32 row0, int32 column1, int32 row1)>;

    TilePaletteGrid();

    void SetTileSet(const GUID& atlasTexture, int32 columns, int32 rows, int32 tileWidthPx,
                    int32 tileHeightPx, std::vector<Cell> cells);
    /** Shows the whole atlas at the largest size that fits the element. */
    void ZoomToFit();
    /** One atlas pixel per logical pixel, within the zoom range. */
    void ZoomToAtlasPixels();
    /** Logical pixels per atlas pixel at the current zoom; 0 with no atlas. */
    float32 AtlasScale() const;
    bool HasHover() const { return m_HoverColumn >= 0 && m_HoverRow >= 0; }
    int32 HoverColumn() const { return m_HoverColumn; }
    int32 HoverRow() const { return m_HoverRow; }
    void SetOnSelectionChanged(SelectionCallback callback);

    bool HasSelection() const { return m_HasSelection; }
    void ClearSelection();
    uint32 TileIdAt(int32 column, int32 row) const;
    /** Whether the cell is part of the selection (always true inside a plain rectangle). */
    bool IsPicked(int32 column, int32 row) const;

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&, float x, float y,
                              float w, float h) override;

  private:
    /** Where the atlas grid sits inside the element, in logical CSS px. */
    struct Geometry
    {
        float32 OriginX = 0.0f;
        float32 OriginY = 0.0f;
        float32 CellW = 0.0f;
        float32 CellH = 0.0f;
        bool Valid = false;
    };

    Geometry ComputeGeometry(float32 x, float32 y, float32 w, float32 h) const;
    /** Cell width at zoom 1: the whole atlas fits the given element size. */
    float32 FitCellWidth(float32 w, float32 h) const;
    bool CellAtPoint(float32 pointX, float32 pointY, int32& outColumn, int32& outRow) const;
    void ApplyZoom(float32 scrollY, float32 pointerX, float32 pointerY);
    /** Fills the pick table with the anchor/cursor rectangle. */
    void PickRectangle();
    /** Recomputes anchor/cursor as the bounds of the picked cells. */
    void FitBoundsToPicks();
    void PublishSelection();

    GUID m_AtlasTexture;
    int32 m_Columns = 0;
    int32 m_Rows = 0;
    int32 m_TileWidthPx = 1;
    // Tile width / tile height in pixels; non-square tiles stay non-square here.
    float32 m_TileAspect = 1.0f;
    std::vector<Cell> m_Cells;

    SelectionCallback m_OnSelectionChanged;

    bool m_HasSelection = false;
    bool m_Dragging = false;
    // One flag per grid cell; the rectangle drag fills it, Ctrl-click edits it.
    std::vector<uint8> m_Picked;
    int32 m_AnchorColumn = 0;
    int32 m_AnchorRow = 0;
    int32 m_CursorColumn = 0;
    int32 m_CursorRow = 0;

    int32 m_HoverColumn = -1;
    int32 m_HoverRow = -1;
    float32 m_Zoom = 1.0f;
    float32 m_PanX = 0.0f;
    float32 m_PanY = 0.0f;
    bool m_Panning = false;
    float32 m_PanAnchorX = 0.0f;
    float32 m_PanAnchorY = 0.0f;
};

} // namespace GameEngine
