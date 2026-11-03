#include "Scripting/TileMapABI.h"

#include "Components/TileMap/TileMapLayerRef.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "DllEngineBootstrap.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "TileMap/TileMapData.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>

namespace
{
using namespace GameEngine;

struct ResolvedLayer
{
    GUID Map;
    uint32 LayerId = 0;
    float32 ZOffset = 0.0f;
    float32 CellWidth = 1.0f;
    float32 CellHeight = 1.0f;
    glm::mat4 LocalToWorld{1.0f};
};

/**
 * Resolves the entity's TileMapLayerRef against the primary world and the
 * runtime store. GE_Result_NotFound when the entity has no layer ref or the
 * map is not resident/resolvable.
 */
GE_Result ResolveLayer(GE_ECS_Entity entity, ResolvedLayer& out)
{
    if (entity == 0)
        return GE_Result_InvalidArg;
    if (DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
        return GE_Result_Fail;

    auto& engine = EngineCore::GetInstance();
    ECS::World* world = engine.EnsurePrimaryWorld();
    if (!world)
        return GE_Result_Fail;

    const ECS::EntityHandle handle(static_cast<uint32>(entity));
    if (!world->IsValid(handle))
        return GE_Result_InvalidArg;

    const auto* layerRef = world->GetComponent<Components::TileMapLayerRef>(handle);
    if (!layerRef || layerRef->TileMap.IsNull() || layerRef->LayerId == 0)
        return GE_Result_NotFound;

    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    const TileMap::TileMapData* data = store ? store->GetMap(layerRef->TileMap.ToGuid()) : nullptr;
    if (!data)
        return GE_Result_NotFound;

    out.Map = layerRef->TileMap.ToGuid();
    out.LayerId = layerRef->LayerId;
    out.ZOffset = layerRef->ZOffset;
    out.CellWidth = data->CellWidth;
    out.CellHeight = data->CellHeight;
    if (const auto* wt = world->GetComponent<Components::WorldTransform>(handle))
        out.LocalToWorld = glm::make_mat4(wt->matrix);
    return GE_Result_Ok;
}

} // namespace

extern "C"
{

GE_API GE_Result GE_CDECL GE_TileMapABI_GetCell(GE_ECS_Entity entity, int32_t cellX, int32_t cellY,
                                                uint32_t* outCell)
{
    try
    {
        if (!outCell)
            return GE_Result_InvalidArg;
        ResolvedLayer layer;
        const GE_Result rc = ResolveLayer(entity, layer);
        if (rc != GE_Result_Ok)
            return rc;
        *outCell = TileMapECS::TileMapRuntimeStore::Get().GetCell(layer.Map, layer.LayerId, cellX,
                                                                  cellY);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_TileMapABI_SetCell(GE_ECS_Entity entity, int32_t cellX, int32_t cellY,
                                                uint32_t cell)
{
    try
    {
        ResolvedLayer layer;
        const GE_Result rc = ResolveLayer(entity, layer);
        if (rc != GE_Result_Ok)
            return rc;
        return TileMapECS::TileMapRuntimeStore::Get().SetCell(layer.Map, layer.LayerId, cellX,
                                                              cellY, cell)
                   ? GE_Result_Ok
                   : GE_Result_Fail;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_TileMapABI_CellToWorld(GE_ECS_Entity entity, int32_t cellX,
                                                    int32_t cellY, float* outWorldX,
                                                    float* outWorldY, float* outWorldZ)
{
    try
    {
        if (!outWorldX || !outWorldY || !outWorldZ)
            return GE_Result_InvalidArg;
        ResolvedLayer layer;
        const GE_Result rc = ResolveLayer(entity, layer);
        if (rc != GE_Result_Ok)
            return rc;
        const glm::vec4 local((static_cast<float32>(cellX) + 0.5f) * layer.CellWidth,
                              (static_cast<float32>(cellY) + 0.5f) * layer.CellHeight,
                              layer.ZOffset, 1.0f);
        const glm::vec4 world = layer.LocalToWorld * local;
        *outWorldX = world.x;
        *outWorldY = world.y;
        *outWorldZ = world.z;
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_TileMapABI_WorldToCell(GE_ECS_Entity entity, float worldX,
                                                    float worldY, float worldZ, int32_t* outCellX,
                                                    int32_t* outCellY)
{
    try
    {
        if (!outCellX || !outCellY)
            return GE_Result_InvalidArg;
        ResolvedLayer layer;
        const GE_Result rc = ResolveLayer(entity, layer);
        if (rc != GE_Result_Ok)
            return rc;
        const glm::vec4 local =
            glm::inverse(layer.LocalToWorld) * glm::vec4(worldX, worldY, worldZ, 1.0f);
        *outCellX = static_cast<int32_t>(std::floor(local.x / layer.CellWidth));
        *outCellY = static_cast<int32_t>(std::floor(local.y / layer.CellHeight));
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_TileMap_GetInterface(uint32_t abiVersion, const void** outTable,
                                                  uint32_t* outSizeBytes)
{
    try
    {
        if (!outTable || !outSizeBytes)
            return GE_Result_InvalidArg;

        const uint32_t major = GE_TILEMAP_ABI_VERSION_MAJOR(abiVersion);
        const uint32_t curMajor = GE_TILEMAP_ABI_VERSION_MAJOR(GE_TILEMAP_ABI_VERSION_CURRENT);
        if (major != curMajor)
        {
            *outTable = nullptr;
            *outSizeBytes = 0;
            return GE_Result_NotFound;
        }

        static GE_TileMap_Interface_v1 s_Iface;
        s_Iface.sizeBytes = sizeof(GE_TileMap_Interface_v1);
        s_Iface.abiVersion = GE_TILEMAP_ABI_VERSION_CURRENT;
        s_Iface.GetCell = &GE_TileMapABI_GetCell;
        s_Iface.SetCell = &GE_TileMapABI_SetCell;
        s_Iface.CellToWorld = &GE_TileMapABI_CellToWorld;
        s_Iface.WorldToCell = &GE_TileMapABI_WorldToCell;

        *outTable = &s_Iface;
        *outSizeBytes = sizeof(GE_TileMap_Interface_v1);
        return GE_Result_Ok;
    }
    catch (...)
    {
        if (outTable)
            *outTable = nullptr;
        if (outSizeBytes)
            *outSizeBytes = 0;
        return GE_Result_Fail;
    }
}

} // extern "C"
