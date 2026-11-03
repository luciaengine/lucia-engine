#pragma once

#include "AssetCore/Types.h"
#include "TileMap/TerrainPeering.h"
#include "TileMap/TileCellEncoding.h"

#include <array>

namespace GameEngine
{
namespace TileMap
{

class TileSetAsset;

/**
 * @brief The terrain a cell should carry and the terrain its eight neighbors
 * carry, as read off the map. -1 is "no terrain" — empty cells, cells painted
 * from another terrain set, and the open plane outside the authored region all
 * report it, so an edge tile is picked the same way whatever produced the gap.
 */
struct TerrainNeighborhood
{
    int16 Center = -1;
    std::array<int16, kPeeringSlotCount> Neighbors{-1, -1, -1, -1, -1, -1, -1, -1};
};

/**
 * @brief Which tile a terrain paint should place. A miss is explicit: the brush
 * leaves the cell alone rather than stamping a tile whose edges are wrong.
 */
struct TerrainMatchResult
{
    uint32 TileId = 0;
    bool Matched = false;
};

/**
 * Picks the tile in `tileSet` whose peering best fits `wanted` within terrain
 * set `terrainSet`.
 *
 * Candidates are the tiles annotated with that terrain set and that center
 * terrain; a set with no such tile is a miss. Among them the tile matching the
 * most peering slots wins, where "the most" is weighed by the set's
 * TerrainMatchMode — Corners ignores the side slots, Sides ignores the corner
 * slots, and CornersAndSides counts sides heavier than corners, since a wrong
 * side edge reads as a seam while a wrong corner reads as a blemish. An exact
 * fit is by construction the maximum score, so it always wins outright.
 *
 * Equal-scoring tiles are resolved by a Probability-weighted pick over `seed`:
 * the same seed and the same tileset always yield the same tile, which is what
 * lets a repaint of the same region reproduce itself.
 */
TerrainMatchResult MatchTerrainTile(const TileSetAsset& tileSet, int16 terrainSet,
                                    const TerrainNeighborhood& wanted, uint32 seed);

/**
 * The terrain a cell already carries within `terrainSet`: the center terrain of
 * the tile it holds, or -1 when the cell is empty, its tile is unknown, or that
 * tile belongs to a different terrain set.
 */
int16 TerrainOfCell(const TileSetAsset& tileSet, TileCell cell, int16 terrainSet);

} // namespace TileMap
} // namespace GameEngine
