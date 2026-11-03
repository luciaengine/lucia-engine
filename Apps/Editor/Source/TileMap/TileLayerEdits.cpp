#include "TileMap/TileLayerEdits.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TileMap/TileMapData.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace GameEngine
{
namespace Editor
{
namespace TileLayerEdits
{

bool LayerStateSnapshot::DiffersFrom(const LayerStateSnapshot& other) const
{
    if (Entries.size() != other.Entries.size())
        return true;
    for (size_t i = 0; i < Entries.size(); ++i)
    {
        const Entry& a = Entries[i];
        const Entry& b = other.Entries[i];
        if (a.Name != b.Name || a.Locked != b.Locked || a.TileSetGuid != b.TileSetGuid ||
            a.Visible != b.Visible || a.ZOffset != b.ZOffset)
            return true;
    }
    return false;
}

ECS::EntityHandle FindLayerEntity(ECS::World& world, const GUID& mapGuid, uint32 layerId)
{
    ECS::EntityHandle found{};
    world.Query<ECS::Read<Components::TileMapLayerRef>>().Each(
        [&](ECS::EntityHandle entity, const Components::TileMapLayerRef& ref)
        {
            if (found.IsValid() || ref.LayerId != layerId || ref.TileMap.ToGuid() != mapGuid)
                return;
            found = entity;
        });
    return found;
}

ECS::EntityHandle CreateLayerEntity(ECS::World& world, const GUID& mapGuid, uint32 layerId,
                                    const std::string& name, ECS::EntityHandle templateEntity)
{
    if (mapGuid.IsNull() || layerId == 0)
        return ECS::EntityHandle{};

    // A template that draws no layer is no template: the new entity then starts
    // from defaults, on the map's plane at the origin.
    const Components::TileMapLayerRef* templateRef =
        templateEntity.IsValid() ? world.GetComponent<Components::TileMapLayerRef>(templateEntity)
                                 : nullptr;

    const ECS::EntityHandle entity = world.CreateEntity();
    if (!entity.IsValid())
        return entity;

    Components::Name entityName{};
    std::memset(entityName.value, 0, sizeof(entityName.value));
    std::strncpy(entityName.value, name.c_str(), sizeof(entityName.value) - 1);
    world.AddComponentImmediate(entity, entityName);

    if (const auto* transform = templateRef != nullptr
                                    ? world.GetComponent<Components::Transform>(templateEntity)
                                    : nullptr)
        world.AddComponentImmediate(entity, *transform);
    else
        world.AddComponentImmediate(entity, Components::Transform{});

    Components::TileMapLayerRef ref =
        templateRef != nullptr ? *templateRef : Components::TileMapLayerRef{};
    ref.TileMap.Set(mapGuid);
    ref.LayerId = layerId;
    if (templateRef != nullptr)
        ref.ZOffset = templateRef->ZOffset - kLayerZStepPx;
    world.AddComponentImmediate(entity, ref);
    return entity;
}

bool CaptureLayerForRestore(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                            const GUID& mapGuid, uint32 layerId, LayerRestoreData& out)
{
    if (mapGuid.IsNull())
        return false;
    const TileMap::TileMapData* map = store.GetMap(mapGuid);
    if (map == nullptr)
        return false;

    const auto& layers = map->Layers();
    for (size_t i = 0; i < layers.size(); ++i)
    {
        if (layers[i].Id != layerId)
            continue;
        out.Map = mapGuid;
        out.Layer = layers[i];
        out.Index = i;
        const ECS::EntityHandle entity = FindLayerEntity(world, mapGuid, layerId);
        if (entity.IsValid())
        {
            if (const auto* ref = world.GetComponent<Components::TileMapLayerRef>(entity))
            {
                out.Ref = *ref;
                out.HasEntity = true;
            }
            if (const auto* name = world.GetComponent<Components::Name>(entity))
                out.EntityName = *name;
            if (const auto* transform = world.GetComponent<Components::Transform>(entity))
            {
                out.Transform = *transform;
                out.HasTransform = true;
            }
        }
        return true;
    }
    return false;
}

void RestoreLayerFrom(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                      const LayerRestoreData& data)
{
    if (data.Map.IsNull())
        return;
    TileMap::TileMapData* map = store.GetMap(data.Map);
    if (map == nullptr)
        return;

    // Original id, original slot: every undo entry recorded below this one refers
    // to layers by id, so a restore under a fresh id would silently orphan them.
    const size_t index = std::min(data.Index, map->Layers().size());
    if (map->FindLayer(data.Layer.Id) == nullptr)
        map->InsertLayer(index, data.Layer);

    if (data.HasEntity && !FindLayerEntity(world, data.Map, data.Layer.Id).IsValid())
    {
        if (const ECS::EntityHandle clone = world.CreateEntity(); clone.IsValid())
        {
            world.AddComponentImmediate(clone, data.EntityName);
            if (data.HasTransform)
                world.AddComponentImmediate(clone, data.Transform);
            world.AddComponentImmediate(clone, data.Ref);
        }
    }

    store.MarkStructuralChange(data.Map);
}

void DestroyLayerAndEntity(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                           const GUID& mapGuid, uint32 layerId)
{
    if (mapGuid.IsNull())
        return;
    TileMap::TileMapData* map = store.GetMap(mapGuid);
    if (map == nullptr)
        return;

    const ECS::EntityHandle entity = FindLayerEntity(world, mapGuid, layerId);
    if (entity.IsValid())
        world.DestroyEntity(entity);
    map->RemoveLayer(layerId);
    store.MarkStructuralChange(mapGuid);
}

LayerStateSnapshot CaptureLayerState(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                                     const GUID& mapGuid)
{
    LayerStateSnapshot snapshot;
    if (mapGuid.IsNull())
        return snapshot;
    const TileMap::TileMapData* map = store.GetMap(mapGuid);
    if (map == nullptr)
        return snapshot;

    snapshot.Map = mapGuid;
    for (const TileMap::TileMapLayerData& layer : map->Layers())
    {
        LayerStateSnapshot::Entry entry;
        entry.LayerId = layer.Id;
        entry.Name = layer.Name;
        entry.Locked = layer.Locked;
        entry.TileSetGuid = layer.TileSetGuid;
        const ECS::EntityHandle entity = FindLayerEntity(world, mapGuid, layer.Id);
        if (const auto* ref = entity.IsValid()
                                  ? world.GetComponent<Components::TileMapLayerRef>(entity)
                                  : nullptr)
        {
            entry.Visible = ref->Visible;
            entry.ZOffset = ref->ZOffset;
            entry.HasEntity = true;
        }
        snapshot.Entries.push_back(std::move(entry));
    }
    return snapshot;
}

void RestoreLayerState(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                       const LayerStateSnapshot& snapshot)
{
    if (snapshot.Map.IsNull())
        return;
    TileMap::TileMapData* map = store.GetMap(snapshot.Map);
    if (map == nullptr)
        return;

    for (const LayerStateSnapshot::Entry& entry : snapshot.Entries)
    {
        if (TileMap::TileMapLayerData* layer = map->FindLayer(entry.LayerId))
        {
            layer->Name = entry.Name;
            layer->Locked = entry.Locked;
            layer->TileSetGuid = entry.TileSetGuid;
        }
        if (!entry.HasEntity)
            continue;
        const ECS::EntityHandle entity = FindLayerEntity(world, snapshot.Map, entry.LayerId);
        if (!entity.IsValid())
            continue;
        if (auto* ref = world.GetComponentForWrite<Components::TileMapLayerRef>(entity))
        {
            ref->Visible = entry.Visible;
            ref->ZOffset = entry.ZOffset;
        }
    }
    store.MarkStructuralChange(snapshot.Map);
}

bool SetLayersTileSet(TileMapECS::TileMapRuntimeStore& store, const GUID& mapGuid,
                      const std::vector<uint32>& layers, const GUID& tileSet)
{
    if (mapGuid.IsNull() || tileSet.IsNull())
        return false;
    TileMap::TileMapData* map = store.GetMap(mapGuid);
    if (map == nullptr)
        return false;
    bool changed = false;
    for (uint32 layerId : layers)
    {
        TileMap::TileMapLayerData* layer = map->FindLayer(layerId);
        if (layer == nullptr || layer->Type != TileMap::TileMapLayerType::Tiles ||
            layer->TileSetGuid == tileSet)
            continue;
        layer->TileSetGuid = tileSet;
        changed = true;
    }
    if (changed)
        store.MarkStructuralChange(mapGuid);
    return changed;
}

bool ApplyLayerOrder(ECS::World& world, TileMapECS::TileMapRuntimeStore& store,
                     const GUID& mapGuid, std::vector<uint32> order, uint32 movedLayer,
                     uint32 targetLayer, bool after)
{
    if (mapGuid.IsNull() || movedLayer == 0)
        return false;

    const auto from = std::find(order.begin(), order.end(), movedLayer);
    if (from == order.end())
        return false;
    order.erase(from);

    auto insertAt = std::find(order.begin(), order.end(), targetLayer);
    if (insertAt == order.end())
        return false;
    if (after)
        ++insertAt;
    order.insert(insertAt, movedLayer);

    // Draw order is ZOffset, not list position. Deal the existing depths back
    // down the new order so the set is preserved and only the assignment moves.
    std::vector<float32> offsets;
    offsets.reserve(order.size());
    for (uint32 layerId : order)
    {
        const ECS::EntityHandle entity = FindLayerEntity(world, mapGuid, layerId);
        const auto* ref = entity.IsValid()
                              ? world.GetComponent<Components::TileMapLayerRef>(entity)
                              : nullptr;
        if (ref == nullptr)
            return false;
        offsets.push_back(ref->ZOffset);
    }
    std::sort(offsets.begin(), offsets.end());

    bool changed = false;
    for (size_t i = 0; i < order.size(); ++i)
    {
        const ECS::EntityHandle entity = FindLayerEntity(world, mapGuid, order[i]);
        if (!entity.IsValid())
            continue;
        if (auto* ref = world.GetComponentForWrite<Components::TileMapLayerRef>(entity))
        {
            if (ref->ZOffset != offsets[i])
            {
                ref->ZOffset = offsets[i];
                changed = true;
            }
        }
    }
    if (changed)
        store.MarkStructuralChange(mapGuid);
    return changed;
}

} // namespace TileLayerEdits
} // namespace Editor
} // namespace GameEngine
