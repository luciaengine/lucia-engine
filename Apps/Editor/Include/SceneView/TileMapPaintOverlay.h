#pragma once

#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SceneViewTools.h"
#include "SceneView/TileMapPaintTarget.h"

#include "AssetCore/Types.h"

#include <vector>

namespace GameEngine
{
namespace Editor
{
class TileMapEditContext;
}

namespace Editor::SceneTools
{

/** Outlines the cells the next stroke would write, in the tilemap's own plane. */
class TileMapCursorGizmo : public IGizmo
{
public:
    void Render(GizmoRenderContext& context) override;

    /** World-space corners of each highlighted cell: 4 per cell, CCW from min. */
    std::vector<float32> CellQuads;
    bool Erasing = false;
    bool Active = false;
};

/**
 * @brief Outlines the marquee selection and the in-progress drag rectangle.
 *
 * Separate from the brush cursor because the two answer different questions —
 * "what will this click write" versus "what region is selected" — and a stroke
 * inside a selection shows both at once.
 */
class TileMapSelectionGizmo : public IGizmo
{
public:
    void Render(GizmoRenderContext& context) override;

    /** World-space corners of the committed selection's outline, CCW from min. */
    std::vector<float32> SelectionQuad;
    /** Same, for the rectangle currently being dragged out. */
    std::vector<float32> PendingQuad;
};

/**
 * @brief Tints IntGrid cells with the colours their values are defined in.
 *
 * Semantic values have no art of their own, so without a tint an IntGrid layer
 * is invisible and unpaintable. Translucent fills rather than outlines, so the
 * dressed tiles underneath still read through.
 */
class TileMapIntGridGizmo : public IGizmo
{
public:
    void Render(GizmoRenderContext& context) override;

    /** One tinted cell: 4 world-space corners, CCW from min, plus its colour. */
    struct TintedCell
    {
        float32 Corners[12]{};
        float32 Color[4]{};
    };

    std::vector<TintedCell> Cells;
};

/**
 * @brief Everything the paint tool draws over the Scene View: the stamp cursor,
 * the marquee outlines, and the IntGrid tint. The tool tells it what changed in
 * cell space; it owns the world-space quads and the gizmos that render them.
 */
class TileMapPaintOverlay
{
public:
    /** Hides the cursor and drops the tint, for when the tool deactivates. */
    void Clear();

    void HideCursor();
    /** Outlines the stamp footprint (or one cell) anchored at the cursor cell. */
    void UpdateCursor(const TileMapPaintTarget& target, const TileMapEditContext& editContext,
                      int32 cellX, int32 cellY, bool erasing);

    void UpdateSelection(const TileMapPaintTarget& target, const TileMapCellRect& selection,
                         const TileMapCellRect& pending);
    void ClearSelection();
    void ClearPending();

    /**
     * Rebuilds the tint from the target layer's IntGrid source. Called every
     * frame the tool is active: the tint has to survive a frame in which the
     * pointer never moved, and an undo that changed it.
     */
    void UpdateIntGridTint(const TileMapPaintTarget& target, const TileMapEditContext* editContext);

    void Gather(GizmoCollector& collector);

private:
    /** Fills a gizmo quad list with the world-space outline of a cell rectangle. */
    static void BuildRectQuad(const TileMapPaintTarget& target, const TileMapCellRect& rect,
                              std::vector<float32>& outQuad);

    TileMapCursorGizmo m_Cursor;
    TileMapSelectionGizmo m_Selection;
    TileMapIntGridGizmo m_IntGridTint;
};

} // namespace Editor::SceneTools
} // namespace GameEngine
