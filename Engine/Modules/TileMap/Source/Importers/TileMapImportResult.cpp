#include "TileMap/Importers/TileMapImportResult.h"

namespace GameEngine
{
namespace TileMap
{
namespace Importers
{

void RemapImportedTileSetGuid(TileMapImportResult& result, const GUID& from, const GUID& to)
{
    for (ImportedTileMap& map : result.Maps)
    {
        if (!map.Asset)
            continue;
        for (TileMapLayerData& layer : map.Asset->Data().Layers())
        {
            if (layer.TileSetGuid == from)
                layer.TileSetGuid = to;
        }
    }
}

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
