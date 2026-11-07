#include "TerrainCollisionReadiness.h"

#include "TerrainECS/TerrainService.h"
#include "TerrainECS/Systems/TerrainPhysicsSystem.h"
#include "TerrainECS/Components/TerrainTileCollider.h"
#include "TerrainECS/Components/TerrainPageCollider.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::TerrainECS
{
namespace
{
bool ReportPlanarTerrain(ECS::World& world, ECS::EntityHandle entity, const Components::Terrain& terrain,
                         TerrainService& service, PhysicsECS::HeightFieldCollisionWait& wait)
{
    const auto* shape = world.GetComponent<Components::HeightFieldColliderShape>(entity);
    const auto* collider = world.GetComponent<Components::PhysicsCollider>(entity);
    const auto* body = world.GetComponent<Components::PhysicsBody>(entity);
    // Collision switched off by the author is absent on purpose, not pending.
    const ECS::Entity terrainEntity(&world, entity);
    if ((shape && !terrainEntity.IsEnabled<Components::HeightFieldColliderShape>()) ||
        (collider && !terrainEntity.IsEnabled<Components::PhysicsCollider>()) ||
        (body && !terrainEntity.IsEnabled<Components::PhysicsBody>()))
        return false;
    if (shape && ColliderMatchesTerrain(*shape, terrain))
        return false;

    const auto* data = service.GetTerrainData({terrain.TerrainDataHandle, terrain.TerrainDataGeneration});
    if (!data || data->Heightfield.IsEmpty() || data->HeightfieldVersion == 0)
        wait = {entity, "terrain height data is not loaded"};
    else if (!shape)
        wait = {entity, "terrain collider is not provisioned yet"};
    else
        wait = {entity, "terrain collider does not match its terrain yet"};
    return true;
}

bool ReportPageColliders(ECS::World& world, ECS::EntityHandle entity, const Components::Terrain& terrain,
                         TerrainService& service, const SurfaceHeightQuery& surface, const Physics::AABB& box,
                         PhysicsECS::HeightFieldCollisionWait& wait)
{
    std::vector<PageStreaming::PageAddress> provisioned;
    world.Query<ECS::Read<Components::TerrainPageCollider>>().Each(
        [&](const Components::TerrainPageCollider& page)
        {
            if (page.TiledIndex == terrain.TiledTerrainHandle && page.TiledGeneration == terrain.TiledTerrainGeneration)
                provisioned.push_back({0, 0, page.PageX, page.PageZ});
        });

    float32 minY, maxY;
    surface.HeightRange(minY, maxY);
    const float32 pageSizeX = surface.LatticeSpacingX() * PageStreaming::kPageOwnedSamples;
    const float32 pageSizeZ = surface.LatticeSpacingZ() * PageStreaming::kPageOwnedSamples;
    for (const auto& page : service.FinestWindowPages(terrain))
    {
        const uint64 firstX = static_cast<uint64>(page.X) * PageStreaming::kPageOwnedSamples;
        const uint64 firstZ = static_cast<uint64>(page.Z) * PageStreaming::kPageOwnedSamples;
        if (firstX + 1u >= surface.LatticeSamplesX() || firstZ + 1u >= surface.LatticeSamplesZ())
            continue;
        const float32 minX = surface.FootprintOriginX() + page.X * pageSizeX;
        const float32 minZ = surface.FootprintOriginZ() + page.Z * pageSizeZ;
        Physics::AABB footprint;
        footprint.min = Physics::Vector3(minX, minY, minZ);
        footprint.max = Physics::Vector3(
            std::min(minX + pageSizeX, surface.FootprintOriginX() + surface.FootprintSizeX()), maxY,
            std::min(minZ + pageSizeZ, surface.FootprintOriginZ() + surface.FootprintSizeZ()));
        if (PhysicsECS::BoxesOverlap(footprint, box) &&
            std::find(provisioned.begin(), provisioned.end(), page) == provisioned.end())
        {
            wait = {entity, "a terrain page's collider is not provisioned yet"};
            return true;
        }
    }
    return false;
}

bool ReportTiledTerrain(ECS::World& world, ECS::EntityHandle entity, const Components::Terrain& terrain,
                        const Components::WorldTransform& transform, TerrainService& service,
                        const Physics::AABB& box, PhysicsECS::HeightFieldCollisionWait& wait)
{
    const TiledTerrainHandle handle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
    auto* tiled = service.GetTiledTerrainData(handle);
    if (!tiled)
    {
        wait = {entity, "tiled terrain data is not loaded"};
        return true;
    }

    const auto surface = service.SurfaceQuery(terrain, transform);
    if (surface.HasPageSource())
        return ReportPageColliders(world, entity, terrain, service, surface, box, wait);

    std::vector<TileCoord> provisioned;
    world.Query<ECS::Read<Components::TerrainTileCollider>>().Each(
        [&](const Components::TerrainTileCollider& tile)
        {
            if (tile.TiledIndex == handle.Index && tile.TiledGeneration == handle.Generation)
                provisioned.push_back(TileCoord{tile.TileX, tile.TileZ});
        });

    const float32 tileSize = tiled->Config.TileWorldSize;
    const float32 baseY = transform.matrix[13];
    for (const auto& [coord, tile] : tiled->Tiles)
    {
        if (!tile || tile->LodState != TileLodState::Full || tile->Heightfield.IsEmpty() ||
            tile->HeightfieldVersion == 0)
            continue;
        Physics::AABB footprint;
        footprint.min = Physics::Vector3(tile->WorldOriginX, baseY, tile->WorldOriginZ);
        footprint.max = Physics::Vector3(tile->WorldOriginX + tileSize, baseY + std::abs(terrain.HeightScale),
                                         tile->WorldOriginZ + tileSize);
        if (!PhysicsECS::BoxesOverlap(footprint, box))
            continue;
        if (std::find(provisioned.begin(), provisioned.end(), coord) == provisioned.end())
        {
            wait = {entity, "a resident terrain tile's collider is not provisioned yet"};
            return true;
        }
    }
    return false;
}
} // namespace

bool FindPendingTerrainCollision(ECS::World& world, const Physics::AABB& box,
                                 PhysicsECS::HeightFieldCollisionWait& wait)
{
    auto* service = TerrainService::TryGet();
    if (!service)
        return false;
    bool pending = false;
    world.Query<ECS::Read<Components::Terrain>, ECS::Optional<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform* transform)
        {
            if (pending || terrain.Domain != Components::TerrainDomain::Planar)
                return;
            const bool tiled = terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0;
            if (!transform)
            {
                pending = true; // Without a placed footprint, disjointness is not established.
                wait = {entity, "terrain has no world transform yet"};
                return;
            }
            if (!tiled)
            {
                Physics::AABB footprint;
                if (PhysicsECS::HeightFieldFootprint(transform->matrix, terrain.SizeX, terrain.SizeZ,
                                                     terrain.HeightScale, footprint) &&
                    !PhysicsECS::BoxesOverlap(footprint, box))
                    return;
                pending = ReportPlanarTerrain(world, entity, terrain, *service, wait);
                return;
            }
            pending = ReportTiledTerrain(world, entity, terrain, *transform, *service, box, wait);
        });
    return pending;
}

} // namespace GameEngine::TerrainECS
