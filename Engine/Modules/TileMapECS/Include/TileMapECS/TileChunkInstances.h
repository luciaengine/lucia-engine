#pragma once

#include "TileMap/TileMapChunk.h"

#include <vector>

namespace GameEngine
{
namespace TileMapECS
{

/**
 * @brief One instanced tile quad. Matches the `TileInstance` std430 struct in
 * tilemap.vert, so a vector of these uploads verbatim.
 */
struct TileInstance
{
    uint32 LocalIndex = 0; // cell index within the chunk (y * kChunkSize + x)
    uint32 Cell = 0;       // packed TileCell, flip/transpose bits preserved

    bool operator==(const TileInstance&) const = default;
};

static_assert(sizeof(TileInstance) == 8, "TileInstance must match the shader's std430 layout");

/**
 * Compacts a chunk's cells into the instance stream: empty cells are dropped,
 * occupied ones keep their packed cell value (flip/transpose bits included).
 * Emitted in row-major cell order. Clears `outInstances` first.
 */
void BuildChunkInstances(const TileMap::TileMapChunk& chunk, std::vector<TileInstance>& outInstances);

} // namespace TileMapECS
} // namespace GameEngine
