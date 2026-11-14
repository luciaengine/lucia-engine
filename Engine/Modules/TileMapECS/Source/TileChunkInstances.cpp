#include "TileMapECS/TileChunkInstances.h"

#include "TileMap/TileCellEncoding.h"

namespace GameEngine
{
namespace TileMapECS
{

void BuildChunkInstances(const TileMap::TileMapChunk& chunk, std::vector<TileInstance>& outInstances)
{
    outInstances.clear();
    for (int32 index = 0; index < TileMap::kChunkCellCount; ++index)
    {
        const TileMap::TileCell cell = chunk.Cells[static_cast<size_t>(index)];
        if (TileMap::CellIsEmpty(cell))
            continue;
        outInstances.push_back(TileInstance{static_cast<uint32>(index), cell});
    }
}

} // namespace TileMapECS
} // namespace GameEngine
