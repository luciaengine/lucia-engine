#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "TileMap/TileCellEncoding.h"

#include <memory>
#include <vector>

namespace GameEngine
{

namespace TileMap
{
class TileSetAsset;
}

namespace Editor
{

/** One cell the terrain resolution wants written. */
struct TerrainCellWrite
{
    int32 CellX = 0;
    int32 CellY = 0;
    TileMap::TileCell Cell = TileMap::kEmptyCell;
};

/**
 * @brief Turns "this cell should be terrain X" into the tile writes that make
 * the map's terrain field consistent again.
 *
 * Painting a terrain changes the correct tile for the painted cell *and* for
 * each of its eight neighbours, because their peering now faces a different
 * terrain. The brush re-resolves that 3x3 block through TerrainMatcher and
 * hands the writes back rather than applying them, so the caller keeps them
 * inside its own undo entry and its own dirty-chunk accounting.
 *
 * The resolved tileset is cached across the strokes of one authoring session:
 * resolving and re-decoding it per painted cell would cost a file read per cell
 * of a drag.
 */
class TileMapTerrainBrush
{
public:
    TileMapTerrainBrush();
    ~TileMapTerrainBrush();

    /**
     * Loads `tileSetGuid` if it is not already loaded and arms `terrainSet`.
     * False when the tileset cannot be resolved or has no such terrain set, in
     * which case the brush stays inactive and paints nothing.
     */
    bool Begin(const GUID& tileSetGuid, int16 terrainSet);

    bool IsActive() const { return m_TileSet != nullptr; }

    /**
     * Assigns `terrain` to (cellX, cellY) — -1 clears the cell — and appends the
     * writes that re-resolve that cell and its eight neighbours. Existing
     * contents of `outWrites` are kept, so a gesture can accumulate.
     */
    void Paint(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY, int16 terrain,
               std::vector<TerrainCellWrite>& outWrites) const;

private:
    /** The terrain field as this paint leaves it: the assignment wins at its own cell. */
    int16 TerrainAt(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY,
                    int32 assignedX, int32 assignedY, int16 assignedTerrain) const;

    bool ResolveCell(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY, int16 terrain,
                     int32 assignedX, int32 assignedY, int16 assignedTerrain,
                     TerrainCellWrite& outWrite) const;

    std::unique_ptr<TileMap::TileSetAsset> m_TileSet;
    GUID m_TileSetGuid;
    int16 m_TerrainSet = -1;
};

} // namespace Editor
} // namespace GameEngine
