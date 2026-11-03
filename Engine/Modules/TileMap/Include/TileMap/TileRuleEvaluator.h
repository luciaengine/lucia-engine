#pragma once

#include "AssetCore/Types.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapChunk.h"

#include <vector>

namespace GameEngine
{
namespace TileMap
{

class TileMapData;

/**
 * @brief One cell a pass moved, with everything needed to put it back.
 *
 * The provenance flag travels with the value because the two are one fact: a
 * cell restored to its tile but not to its flag would be silently re-dressable
 * afterwards.
 */
struct RuleCellChange
{
    CellCoord Cell;
    TileCell Before = kEmptyCell;
    TileCell After = kEmptyCell;
    bool AutoBefore = false;
    bool AutoAfter = false;
};

/**
 * Re-dresses the Tiles layer `rulesLayerId` from the IntGrid layer its rule set
 * names, over the whole authored region.
 *
 * The region is every chunk the source IntGrid layer holds, dilated by one chunk
 * so patterns reaching across a chunk seam still see their inputs, plus every
 * chunk that already holds auto-dressed cells so art whose IntGrid support was
 * erased is cleared rather than stranded.
 *
 * Hand-painted cells are never touched: a cell that holds a tile and is not
 * flagged auto is skipped, both as a target and as a stamp destination.
 *
 * Cells whose value actually changed are appended to `outChanged` in scan order
 * (ascending Y, then ascending X). Returns false — writing nothing — when the
 * layer, its type, or its source IntGrid layer does not resolve.
 */
bool EvaluateRules(TileMapData& map, uint32 rulesLayerId, std::vector<RuleCellChange>& outChanged);

/**
 * Re-dresses only the cells whose result can depend on `dirtyCells`.
 *
 * A cell's dressing reads the IntGrid within one pattern radius, and a stamp
 * from a cell one radius away reads one radius further again — so an edit can
 * move a cell two radii away, and computing those cells means evaluating the
 * potential stampers one radius beyond them.
 *
 * The result is identical to EvaluateRules over the same map for any dirty set
 * that covers the IntGrid edits made since the last pass.
 */
bool EvaluateRulesIncremental(TileMapData& map, uint32 rulesLayerId,
                              const std::vector<CellCoord>& dirtyCells,
                              std::vector<RuleCellChange>& outChanged);

} // namespace TileMap
} // namespace GameEngine
