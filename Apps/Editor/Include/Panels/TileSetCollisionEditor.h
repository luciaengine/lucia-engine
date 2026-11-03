#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "Mathematics/Vector2.h"
#include "UI/UIElement.h"

#include <functional>
#include <vector>

namespace GameEngine
{

namespace TileMap
{
class TileSetAsset;
}

/**
 * @brief The collision authoring canvas: the tileset's tiles laid out flush on
 * their slice grid (stride = tile size, no borders), with each tile's collision
 * polygons drawn as a translucent overlay.
 *
 * Left click selects a tile; clicking the tile that is already selected cycles
 * it between no collision and the full-square default. Selecting first keeps a
 * click that only meant "show me this tile's mode and material" from wiping the
 * outline it was asking about. Right clicks drop custom outline points into a
 * tile (snapped to a fine grid); from the third point on, the outline replaces
 * the tile's collision live, and every further point extends it. The selected
 * tile carries a selection outline overlay, and a one-way tile carries a marker
 * on the edge bodies may pass through.
 *
 * Edits go into the panel's own TileSetAsset through a borrowed pointer and are
 * reported per change; writing the file stays with the owning panel.
 */
class TileSetCollisionEditor : public UIElement
{
public:
    TileSetCollisionEditor();

    /** The asset to author. Borrowed — it must outlive this element. Null clears the canvas. */
    void SetTileSet(TileMap::TileSetAsset* tileSet);

    /** Re-reads the tileset after the panel re-sliced or reloaded it. */
    void Refresh();

    void SetOnCollisionChanged(std::function<void()> callback);

    /** Fires when the selected tile changes, including when it becomes none. */
    void SetOnSelectionChanged(std::function<void()> callback);

    /** The selected tile's 1-based id, or 0 when nothing is selected. */
    uint32 SelectedTileId() const;

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, float x,
                              float y, float w, float h) override;

private:
    /** Where the tile grid sits inside the element, in logical CSS px. */
    struct Geometry
    {
        float32 OriginX = 0.0f;
        float32 OriginY = 0.0f;
        float32 CellW = 0.0f;
        float32 CellH = 0.0f;
        bool Valid = false;
    };

    Geometry ComputeGeometry(float32 x, float32 y, float32 w, float32 h) const;

    /**
     * Resolves a pointer position to a tile and the position within it, in
     * tile-local [0,1]² with +Y up (the collision polygon space).
     */
    bool TileAtPoint(float32 pointX, float32 pointY, int32& outColumn, int32& outRow,
                     Mathematics::Vector2& outLocal) const;

    void SelectTile(int32 column, int32 row);
    void ToggleFullSquare(int32 column, int32 row);
    void AddDraftPoint(int32 column, int32 row, Mathematics::Vector2 local);
    void CommitDraftIfClosed();

    void ApplyZoom(float32 scrollY, float32 pointerX, float32 pointerY);

    uint32 TileIdAt(int32 column, int32 row) const;

    TileMap::TileSetAsset* m_TileSet = nullptr;
    GUID m_AtlasTexture;
    int32 m_Columns = 0;
    int32 m_Rows = 0;
    // Tile width / tile height in pixels; non-square tiles stay non-square here.
    float32 m_TileAspect = 1.0f;

    std::function<void()> m_OnCollisionChanged;
    std::function<void()> m_OnSelectionChanged;

    // The tile whose mode and material the side panel edits (-1 = none).
    int32 m_SelectedColumn = -1;
    int32 m_SelectedRow = -1;

    // Custom-outline draft: the tile it belongs to (-1 = none) and its points
    // in tile-local [0,1]² with +Y up.
    int32 m_DraftColumn = -1;
    int32 m_DraftRow = -1;
    std::vector<Mathematics::Vector2> m_DraftPoints;

    float32 m_Zoom = 1.0f;
    float32 m_PanX = 0.0f;
    float32 m_PanY = 0.0f;
    bool m_Panning = false;
    float32 m_PanAnchorX = 0.0f;
    float32 m_PanAnchorY = 0.0f;
};

} // namespace GameEngine
