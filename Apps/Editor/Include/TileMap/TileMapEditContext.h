#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "ECS/ECS.h"
#include "TileMap/TileCellEncoding.h"

#include <vector>

namespace GameEngine
{
namespace Editor
{

/** Which edit a pointer stroke in the Scene View performs. */
enum class TileMapTool : uint8
{
    Brush = 0,
    Eraser,
    Picker,
    Fill,
    Rect,
    Marquee,
    Terrain,
    IntGrid
};

/** The "Tile Map Tool" Shortcuts-catalog entry that arms the tool. */
const char* TileMapToolShortcutName(TileMapTool tool);

/**
 * @brief The rectangular block of cells a stroke writes, as picked in the
 * palette. A one-tile selection is the ordinary brush; a multi-tile selection
 * stamps its whole footprint anchored at the cursor cell.
 *
 * Cells are row-major from the top-left, matching the palette's own layout, so
 * the stamp reads the same way it was selected.
 */
struct TileStamp
{
    int32 Width = 0;
    int32 Height = 0;
    std::vector<TileMap::TileCell> Cells;

    bool IsEmpty() const;

    /** Cell at (column, row), or empty when out of range. */
    TileMap::TileCell At(int32 column, int32 row) const;
};

/**
 * @brief The editor-wide tilemap authoring state: which tileset the palette
 * shows, what the next stroke stamps, which tool is armed, and which map layer
 * receives the edit.
 *
 * The palette panel writes it, the Scene View tool reads it, and the inspector
 * retargets it. Observers poll Version() rather than registering callbacks —
 * panels already run a per-frame Update, and a version compare cannot dangle
 * when a panel is destroyed mid-edit.
 */
class TileMapEditContext
{
public:
    const GUID& TileSet() const { return m_TileSet; }
    void SetTileSet(const GUID& tileSet);

    const TileStamp& Stamp() const { return m_Stamp; }
    void SetStamp(TileStamp stamp);

    TileMapTool Tool() const { return m_Tool; }
    void SetTool(TileMapTool tool);

    /**
     * Paint a weighted-random tile from the stamp instead of its footprint,
     * using each tile's authored Probability. Ignored by a single-tile stamp.
     */
    bool RandomScatter() const { return m_RandomScatter; }
    void SetRandomScatter(bool enabled);

    /**
     * What the Terrain tool paints: an index into the tileset's terrain sets,
     * and a terrain within it. A terrain of -1 is the eraser of terrain
     * painting — it clears the cell and repairs its neighbours — so it is a
     * selectable choice, not an absent one. A terrain set of -1 disarms terrain
     * painting entirely.
     */
    int16 TerrainSet() const { return m_TerrainSet; }
    int16 Terrain() const { return m_Terrain; }
    void SetTerrain(int16 terrainSet, int16 terrain);

    /**
     * What the IntGrid tool writes. Value 0 is the eraser of semantic painting,
     * so it is a selectable choice rather than an absent one.
     */
    uint8 IntGridValue() const { return m_IntGridValue; }
    void SetIntGridValue(uint8 value);

    /** Whether the Scene View tints IntGrid cells with their value colours. */
    bool ShowIntGridTint() const { return m_ShowIntGridTint; }
    void SetShowIntGridTint(bool show);

    /**
     * Where a stroke writes: the entity whose TileMapLayerRef draws the layer,
     * the .tilemap asset, and the stable layer id. The context is the one owner
     * of this choice — the selection feeds it through the editor integration,
     * a palette row click overrides it, and the Scene View tool only reads it.
     * A layer no entity draws can be targeted (the palette lists it) but not
     * painted: the plane a stroke lands on is the entity's transform.
     */
    ECS::EntityHandle TargetEntity() const { return m_TargetEntity; }
    const GUID& TargetMap() const { return m_TargetMap; }
    uint32 TargetLayer() const { return m_TargetLayer; }
    void SetTarget(ECS::EntityHandle entity, const GUID& mapGuid, uint32 layerId);
    void ClearTarget();

    /** True when a map layer is targeted, whether or not an entity draws it. */
    bool HasTargetLayer() const;
    /** True when a stroke has an entity, a map and a layer to write to. */
    bool HasPaintTarget() const;

    /** Rotates/mirrors every cell of the stamp, footprint included. */
    void FlipStampHorizontal();
    void FlipStampVertical();
    void RotateStampClockwise();

    /** Bumped by every accepted mutation; observers compare against it. */
    uint64 Version() const { return m_Version; }

private:
    GUID m_TileSet;
    TileStamp m_Stamp;
    TileMapTool m_Tool = TileMapTool::Brush;
    bool m_RandomScatter = false;
    int16 m_TerrainSet = -1;
    int16 m_Terrain = -1;
    uint8 m_IntGridValue = 1;
    bool m_ShowIntGridTint = true;
    ECS::EntityHandle m_TargetEntity;
    GUID m_TargetMap;
    uint32 m_TargetLayer = 0;
    uint64 m_Version = 0;
};

} // namespace Editor
} // namespace GameEngine
