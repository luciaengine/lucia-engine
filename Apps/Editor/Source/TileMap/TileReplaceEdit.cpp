#include "TileMap/TileReplaceEdit.h"

#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapChunk.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <utility>

namespace GameEngine
{
namespace Editor
{

std::unique_ptr<TileMapEditCommand> ReplaceTileEverywhere(TileMapECS::TileMapRuntimeStore& store,
                                                          const GUID& mapGuid,
                                                          const std::vector<uint32>& layers,
                                                          uint32 fromTileId, uint32 toTileId,
                                                          size_t& outCells)
{
    outCells = 0;
    if (mapGuid.IsNull() || fromTileId == 0 || fromTileId == toTileId)
        return nullptr;
    TileMap::TileMapData* map = store.GetMap(mapGuid);
    if (map == nullptr)
        return nullptr;

    std::vector<TileMapEditCommand::LayerEdit> edits;
    for (uint32 layerId : layers)
    {
        const TileMap::TileMapLayerData* layer = map->FindLayer(layerId);
        if (layer == nullptr || layer->Locked)
            continue;

        // Gather first, write second: a write can drop an emptied chunk, and the
        // chunk list being walked must not change under the walk.
        TileMapEditCommand::LayerEdit edit;
        edit.LayerId = layerId;
        for (const TileMap::ChunkCoord chunk : layer->SortedChunkCoords())
        {
            for (int32 local = 0; local < TileMap::kChunkCellCount; ++local)
            {
                const int32 cellX = chunk.X * TileMap::kChunkSize + (local % TileMap::kChunkSize);
                const int32 cellY = chunk.Y * TileMap::kChunkSize + (local / TileMap::kChunkSize);
                const TileMap::TileCell before = layer->GetCell(cellX, cellY);
                if (TileMap::CellTileId(before) != fromTileId)
                    continue;
                TileMapEditCommand::CellDelta delta{};
                delta.CellX = cellX;
                delta.CellY = cellY;
                delta.Before = before;
                delta.After = toTileId == 0
                                  ? TileMap::kEmptyCell
                                  : TileMap::MakeCell(toTileId, TileMap::CellFlipH(before),
                                                      TileMap::CellFlipV(before),
                                                      TileMap::CellTranspose(before));
                delta.AutoBefore = layer->IsAutoCell(cellX, cellY);
                // A replaced cell is authored: rule evaluation must not undo it.
                delta.AutoAfter = false;
                edit.Cells.push_back(delta);
            }
        }
        if (!edit.Cells.empty())
            edits.push_back(std::move(edit));
    }
    if (edits.empty())
        return nullptr;

    for (const TileMapEditCommand::LayerEdit& edit : edits)
    {
        for (const TileMapEditCommand::CellDelta& delta : edit.Cells)
        {
            store.SetCellAuto(mapGuid, edit.LayerId, delta.CellX, delta.CellY, delta.After,
                              delta.AutoAfter);
            ++outCells;
        }
    }
    return std::make_unique<TileMapEditCommand>("Replace Tile", mapGuid, std::move(edits));
}

} // namespace Editor
} // namespace GameEngine
