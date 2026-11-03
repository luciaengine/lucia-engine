#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "Components/Name.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "TileMap/TileMapLayerData.h"

#include <cstddef>
#include <string>
#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace TileMapECS
{
class TileMapRuntimeStore;
}

namespace Editor
{

/**
 * @brief The layer-level edits the tile palette performs, on the map in the
 * runtime store and on the entities that draw its layers.
 *
 * A tilemap layer lives in two places: its cells, name and lock in the
 * `.tilemap` asset, and its visibility, depth and transform on the scene entity
 * whose TileMapLayerRef draws it. Every operation here keeps the two in step,
 * and none of them touches UI, so the undo commands and the palette's layer
 * list share one implementation. Callers rebuild their views from the store's
 * StructureVersion, which every mutating operation bumps.
 */
namespace TileLayerEdits
{

// Half a layer step: a new or duplicated layer sits just in front of its
// template without colliding with the next authored layer's ZOffset.
constexpr float32 kLayerZStepPx = 0.05f;

/**
 * Everything needed to bring a deleted layer back: the layer itself (a value
 * copy, so its cells come with it), where it sat, and the components of the
 * entity that drew it. Entity handles are not stable across destroy/recreate,
 * so the entity is rebuilt from its components rather than resurrected.
 */
struct LayerRestoreData
{
    GUID Map;
    TileMap::TileMapLayerData Layer;
    size_t Index = 0;
    bool HasEntity = false;
    Components::TileMapLayerRef Ref{};
    Components::Name EntityName{};
    Components::Transform Transform{};
    bool HasTransform = false;
};

/**
 * Snapshot of every layer's editable state. Taken before an edit and again
 * after, the pair is the undo entry -- one shape covers visibility, lock,
 * rename and reorder rather than a command class per control.
 */
struct LayerStateSnapshot
{
    GUID Map;
    struct Entry
    {
        uint32 LayerId = 0;
        std::string Name;
        bool Locked = false;
        GUID TileSetGuid;
        bool Visible = true;
        float32 ZOffset = 0.0f;
        bool HasEntity = false;
    };
    std::vector<Entry> Entries;

    /** True when any entry differs from `other` in name, lock, tileset, visibility or depth. */
    bool DiffersFrom(const LayerStateSnapshot& other) const;
};

/**
 * The entity in this scene that renders (map, layerId). Visibility, opacity and
 * draw order are per-entity, so a layer row has to reach its own entity rather
 * than whichever one the user happens to have selected.
 */
ECS::EntityHandle FindLayerEntity(ECS::World& world, const GUID& mapGuid, uint32 layerId);

/**
 * Creates the entity that draws `layerId`, just in front of `templateEntity`
 * (whose transform and layer settings it copies when valid). A layer with no
 * entity is invisible and unpaintable, so every path that adds a layer to the
 * asset also adds its entity to the scene.
 */
ECS::EntityHandle CreateLayerEntity(ECS::World& world, const GUID& mapGuid, uint32 layerId,
                                    const std::string& name, ECS::EntityHandle templateEntity);

bool CaptureLayerForRestore(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                            const GUID& mapGuid, uint32 layerId, LayerRestoreData& out);
void RestoreLayerFrom(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                      const LayerRestoreData& data);
void DestroyLayerAndEntity(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                           const GUID& mapGuid, uint32 layerId);

LayerStateSnapshot CaptureLayerState(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                                     const GUID& mapGuid);
void RestoreLayerState(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                       const LayerStateSnapshot& snapshot);

/**
 * Binds `tileSet` to every Tiles layer in `layers`. Cell ids are kept as they
 * are: a swap is for a tileset laid out like the old one (a re-skin), and the
 * ids simply resolve through the new atlas. Returns true when any layer changed.
 */
bool SetLayersTileSet(TileMapECS::TileMapRuntimeStore& store, const GUID& mapGuid,
                      const std::vector<uint32>& layers, const GUID& tileSet);

/**
 * Moves `movedLayer` before or after `targetLayer` in `order` (frontmost first)
 * and deals the layers' existing depths back down the new order, so the set of
 * ZOffsets is preserved and only their assignment changes. Returns true when
 * any depth moved.
 */
bool ApplyLayerOrder(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                     const GUID& mapGuid, std::vector<uint32> order, uint32 movedLayer,
                     uint32 targetLayer, bool after);

} // namespace TileLayerEdits
} // namespace Editor
} // namespace GameEngine
