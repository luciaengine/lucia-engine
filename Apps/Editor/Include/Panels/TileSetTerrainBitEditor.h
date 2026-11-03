#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "UI/UIElement.h"

#include <functional>

namespace GameEngine
{

namespace TileMap
{
class TileSetAsset;
}

/**
 * @brief The terrain-bit authoring canvas: the tileset's tiles laid out on their
 * slice grid, each subdivided into the nine regions of its peering annotation —
 * the centre terrain and the eight neighbour slots — painted in the terrains'
 * own colours.
 *
 * Dragging with the left button paints the armed terrain into every region the
 * pointer crosses; the right button clears them. A tile whose centre and all
 * eight slots are cleared leaves terrain matching altogether.
 *
 * It edits the panel's own TileSetAsset in place through a borrowed pointer and
 * reports each change, leaving the decision of when to write the file to the
 * panel that owns both.
 */
class TileSetTerrainBitEditor : public UIElement
{
public:
    TileSetTerrainBitEditor();

    /** The asset to author. Borrowed — it must outlive this element. Null clears the canvas. */
    void SetTileSet(TileMap::TileSetAsset* tileSet);

    /** Re-reads the tileset after the panel re-sliced or reloaded it. */
    void Refresh();

    /** What a left-button paint writes. A terrain of -1 makes the left button clear too. */
    void SetActiveTerrain(int16 terrainSet, int16 terrain);

    void SetOnPeeringChanged(std::function<void()> callback);

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
     * Resolves a pointer position to a tile and one of its nine regions. The
     * region index is the peering slot, or kCenterRegion for the tile's own
     * terrain.
     */
    bool RegionAtPoint(float32 pointX, float32 pointY, int32& outColumn, int32& outRow,
                       int32& outRegion) const;

    /** Writes `terrain` into one region, dropping the tile out of terrain matching when it empties. */
    void PaintRegion(int32 column, int32 row, int32 region, int16 terrain);

    void ApplyZoom(float32 scrollY, float32 pointerX, float32 pointerY);

    TileMap::TileSetAsset* m_TileSet = nullptr;
    GUID m_AtlasTexture;
    int32 m_Columns = 0;
    int32 m_Rows = 0;
    // Tile width / tile height in pixels; non-square tiles stay non-square here.
    float32 m_TileAspect = 1.0f;

    int16 m_ActiveTerrainSet = -1;
    int16 m_ActiveTerrain = -1;

    std::function<void()> m_OnPeeringChanged;

    float32 m_Zoom = 1.0f;
    float32 m_PanX = 0.0f;
    float32 m_PanY = 0.0f;
    bool m_Panning = false;
    float32 m_PanAnchorX = 0.0f;
    float32 m_PanAnchorY = 0.0f;
    // Which button is painting, so a drag keeps writing the same value; -1 = idle.
    int32 m_PaintingButton = -1;
};

} // namespace GameEngine
