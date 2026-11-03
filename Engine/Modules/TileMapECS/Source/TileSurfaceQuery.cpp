#include "TileMapECS/TileSurfaceQuery.h"

#include "PhysicsECS/ContactSurfaceTable.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapData.h"
#include "TileMapECS/TileMapRuntimeStore.h"

namespace GameEngine
{
namespace TileMapECS
{

TileSurface QueryTileSurface(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY)
{
    TileSurface surface;

    TileMapRuntimeStore* store = TileMapRuntimeStore::TryGet();
    if (!store)
        return surface;

    const TileMap::TileCell cell = store->GetCell(mapGuid, layerId, cellX, cellY);
    const uint32 tileId = TileMap::CellTileId(cell);
    if (tileId == 0)
        return surface;
    surface.HasTile = true;

    const TileMap::TileMapData* data = store->GetMap(mapGuid);
    const TileMap::TileMapLayerData* layer = data ? data->FindLayer(layerId) : nullptr;
    const TileMap::TileSetAsset* tileSet = layer ? store->GetTileSet(layer->TileSetGuid) : nullptr;
    const TileMap::TileSetTile* tile = tileSet ? tileSet->FindTile(tileId) : nullptr;
    if (!tile)
        return surface;

    surface.HasCollision = !tile->CollisionPolygons.empty();
    surface.Mode = tile->CollisionMode;

    // Resolution goes through the same table the baked bodies index into, so a
    // swept character and a Jolt contact read one material, not two.
    const uint32 index = PhysicsECS::ContactSurfaceTable::ResolveMaterial(tile->PhysicsMaterial);
    if (index != PhysicsECS::ContactSurfaceTable::kNoMaterial)
    {
        surface.HasMaterial = true;
        surface.Material = PhysicsECS::ContactSurfaceTable::MaterialAt(index);
    }
    return surface;
}

} // namespace TileMapECS
} // namespace GameEngine
