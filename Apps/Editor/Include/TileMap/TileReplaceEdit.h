#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "TileMap/TileMapEditCommand.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace GameEngine
{
namespace TileMapECS
{
class TileMapRuntimeStore;
}

namespace Editor
{

/**
 * Rewrites every cell of `layers` that holds `fromTileId` to hold `toTileId`
 * (0 clears the cell), keeping each cell's flip and transpose bits. The write
 * is applied to the store as it goes; the returned command is the undo entry,
 * already in its applied state, or null when no cell matched. `outCells` is
 * how many cells changed.
 */
std::unique_ptr<TileMapEditCommand> ReplaceTileEverywhere(TileMapECS::TileMapRuntimeStore& store,
                                                          const GUID& mapGuid,
                                                          const std::vector<uint32>& layers,
                                                          uint32 fromTileId, uint32 toTileId,
                                                          size_t& outCells);

} // namespace Editor
} // namespace GameEngine
