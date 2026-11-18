#include "TileMapECS/TileMapPixelGrid.h"

#include "Components/TileMap/TileMapLayerRef.h"
#include "ECS/ECSTemplates.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapRuntimeStore.h"

namespace GameEngine
{
namespace TileMapECS
{
namespace
{

/** The layer that supplies the scene density: the lowest (tile map GUID, layer id). */
struct PixelGridSource
{
    GUID Map;
    uint32 LayerId = 0;

    bool IsValid() const { return LayerId != 0; }

    bool Precedes(const PixelGridSource& other) const
    {
        if (!other.IsValid())
            return true;
        if (!(Map == other.Map))
            return Map < other.Map;
        return LayerId < other.LayerId;
    }
};

} // namespace

float32 ResolveScenePixelsPerUnit(ECS::World& world)
{
    TileMapRuntimeStore* store = TileMapRuntimeStore::TryGet();
    if (!store)
        return kUnresolvedPixelsPerUnit;

    PixelGridSource source;
    world.Query<ECS::Read<Components::TileMapLayerRef>>().Without<ECS::Disabled>().Each(
        [&source](const Components::TileMapLayerRef& layerRef)
        {
            if (!layerRef.Visible || layerRef.LayerId == 0 || layerRef.TileMap.IsNull())
                return;
            const PixelGridSource candidate{layerRef.TileMap.ToGuid(), layerRef.LayerId};
            if (candidate.Precedes(source))
                source = candidate;
        });
    if (!source.IsValid())
        return kUnresolvedPixelsPerUnit;

    const TileMap::TileMapData* map = store->GetMap(source.Map);
    if (!map)
        return kUnresolvedPixelsPerUnit;
    const TileMap::TileMapLayerData* layer = map->FindLayer(source.LayerId);
    if (!layer || layer->TileSetGuid.IsNull())
        return kUnresolvedPixelsPerUnit;
    const TileMap::TileSetAsset* tileSet = store->GetTileSet(layer->TileSetGuid);
    if (!tileSet)
        return kUnresolvedPixelsPerUnit;
    return DerivePixelsPerUnit(tileSet->TileWidthPx(), map->CellWidth);
}

} // namespace TileMapECS
} // namespace GameEngine
