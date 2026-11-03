#include "Engine/Rendering/TileMapCameraPixelGrid.h"

#include "Components/Rendering/Camera.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapPixelGrid.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::Engine::Renderer
{

namespace
{

/**
 * The layer a scene's pixel grid is derived from: the lowest (tilemap GUID,
 * layer id) among the visible ones.
 *
 * Any rule would do — what matters is that it does not depend on ECS iteration
 * order. Taking whichever layer the query visited first made the camera's pixel
 * grid, its padded render-target size, its zoom and its snap grid flip whenever
 * an archetype move or an entity destruction reshuffled the chunks, in a scene
 * holding two tilemaps of different densities.
 */
struct PixelGridSource
{
    GUID Map;
    uint32 LayerId = 0;

    bool IsValid() const { return LayerId != 0; }
    bool PrecedesIn(const PixelGridSource& other) const
    {
        if (!other.IsValid())
            return true;
        if (!(Map == other.Map))
            return Map < other.Map;
        return LayerId < other.LayerId;
    }
};

} // namespace

float ResolveScenePixelsPerUnit(ECS::World& world)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store)
        return TileMapECS::kUnresolvedPixelsPerUnit;

    PixelGridSource source;
    world.Query<ECS::Read<Components::TileMapLayerRef>>().Without<ECS::Disabled>().Each(
        [&](const Components::TileMapLayerRef& layerRef)
        {
            if (!layerRef.Visible || layerRef.LayerId == 0 || layerRef.TileMap.IsNull())
                return;
            const PixelGridSource candidate{layerRef.TileMap.ToGuid(), layerRef.LayerId};
            if (candidate.PrecedesIn(source))
                source = candidate;
        });
    if (!source.IsValid())
        return TileMapECS::kUnresolvedPixelsPerUnit;

    // The runtime store caches resident maps and tilesets and owns their reload
    // lifecycle. Read those entries directly rather than keeping a second cache
    // that could outlive a project and alias reused GUIDs and edit versions.
    float pixelsPerUnit = TileMapECS::kUnresolvedPixelsPerUnit;
    if (const TileMap::TileMapData* map = store->GetMap(source.Map))
    {
        const TileMap::TileMapLayerData* layer = map->FindLayer(source.LayerId);
        if (layer && !layer->TileSetGuid.IsNull())
        {
            if (const TileMap::TileSetAsset* tileSet = store->GetTileSet(layer->TileSetGuid))
                pixelsPerUnit =
                    TileMapECS::DerivePixelsPerUnit(tileSet->TileWidthPx(), map->CellWidth);
        }
    }

    return pixelsPerUnit;
}

uint32_t ResolvePixelPerfectPixelsPerUnit(ECS::World& world, const Components::Camera& camera)
{
    const uint32_t authored = std::max(1u, static_cast<uint32_t>(camera.PixelPerfectPixelsPerUnit));
    if (!camera.PixelPerfectAutoPixelsPerUnit)
        return authored;

    const float resolved = ResolveScenePixelsPerUnit(world);
    if (!(resolved > 0.0f) || !std::isfinite(resolved) ||
        static_cast<double>(resolved) > std::numeric_limits<uint32_t>::max())
        return authored;

    // Round rather than truncate: a tileset whose density lands a hair under an
    // integer (16 px on a cell stored as 1.0000001) is meant to be that integer,
    // and truncating would reduce the density to 15. Use the 64-bit result:
    // Windows long cannot represent the upper half of the uint32 density range.
    return std::max(1u, static_cast<uint32_t>(std::llround(resolved)));
}

} // namespace GameEngine::Engine::Renderer
