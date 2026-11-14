#include "TileMap/TileMapTerrainBrush.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "TileMap/TerrainMatcher.h"
#include "TileMap/TerrainPeering.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <limits>

namespace GameEngine
{
namespace Editor
{
namespace
{

bool NeighborCell(int32 cellX, int32 cellY, TileMap::PeeringSlotOffset offset,
                  int32& neighborX, int32& neighborY)
{
    const int64 x = static_cast<int64>(cellX) + offset.X;
    const int64 y = static_cast<int64>(cellY) + offset.Y;
    if (x < std::numeric_limits<int32>::min() || x > std::numeric_limits<int32>::max() ||
        y < std::numeric_limits<int32>::min() || y > std::numeric_limits<int32>::max())
        return false;
    neighborX = static_cast<int32>(x);
    neighborY = static_cast<int32>(y);
    return true;
}

/**
 * The tie-break seed is a function of the cell alone, so repainting a region
 * reproduces the same variant tiles instead of reshuffling them under the user.
 */
uint32 CellSeed(int32 cellX, int32 cellY)
{
    return static_cast<uint32>(cellX) * 0x9E3779B9u ^ (static_cast<uint32>(cellY) * 0x85EBCA6Bu);
}

} // namespace

TileMapTerrainBrush::TileMapTerrainBrush() = default;
TileMapTerrainBrush::~TileMapTerrainBrush() = default;

bool TileMapTerrainBrush::Begin(const GUID& tileSetGuid, int16 terrainSet)
{
    m_TerrainSet = -1;
    if (tileSetGuid.IsNull() || terrainSet < 0)
    {
        m_TileSet.reset();
        m_TileSetGuid = GUID();
        return false;
    }

    if (!m_TileSet || m_TileSetGuid != tileSetGuid)
    {
        m_TileSet.reset();
        m_TileSetGuid = GUID();

        auto& engine = EngineCore::GetInstance();
        AssetMetadata meta{};
        if (!engine.IsInitialized() ||
            !engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(tileSetGuid, meta) ||
            meta.Path.empty())
        {
            return false;
        }
        auto candidate = std::make_unique<TileMap::TileSetAsset>(tileSetGuid, meta.Path);
        if (!candidate->Load())
            return false;
        m_TileSet = std::move(candidate);
        m_TileSetGuid = tileSetGuid;
    }

    if (static_cast<size_t>(terrainSet) >= m_TileSet->TerrainSets().size())
        return false;
    m_TerrainSet = terrainSet;
    return true;
}

int16 TileMapTerrainBrush::TerrainAt(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY,
                                     int32 assignedX, int32 assignedY, int16 assignedTerrain) const
{
    if (cellX == assignedX && cellY == assignedY)
        return assignedTerrain;
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store)
        return -1;
    return TileMap::TerrainOfCell(*m_TileSet, store->GetCell(mapGuid, layerId, cellX, cellY),
                                  m_TerrainSet);
}

bool TileMapTerrainBrush::ResolveCell(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY,
                                      int16 terrain, int32 assignedX, int32 assignedY,
                                      int16 assignedTerrain, TerrainCellWrite& outWrite) const
{
    TileMap::TerrainNeighborhood wanted;
    wanted.Center = terrain;
    for (uint32 slot = 0; slot < TileMap::kPeeringSlotCount; ++slot)
    {
        const TileMap::PeeringSlotOffset offset = TileMap::kPeeringSlotOffsets[slot];
        int32 neighborX = 0;
        int32 neighborY = 0;
        wanted.Neighbors[slot] = NeighborCell(cellX, cellY, offset, neighborX, neighborY)
            ? TerrainAt(mapGuid, layerId, neighborX, neighborY, assignedX, assignedY, assignedTerrain)
            : -1;
    }

    const TileMap::TerrainMatchResult match =
        TileMap::MatchTerrainTile(*m_TileSet, m_TerrainSet, wanted, CellSeed(cellX, cellY));
    if (!match.Matched)
        return false;

    outWrite.CellX = cellX;
    outWrite.CellY = cellY;
    outWrite.Cell = TileMap::MakeCell(match.TileId);
    return true;
}

void TileMapTerrainBrush::Paint(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY,
                                int16 terrain, std::vector<TerrainCellWrite>& outWrites) const
{
    if (!IsActive() || m_TerrainSet < 0)
        return;

    if (terrain < 0)
    {
        outWrites.push_back(TerrainCellWrite{cellX, cellY, TileMap::kEmptyCell});
    }
    else
    {
        TerrainCellWrite write;
        if (ResolveCell(mapGuid, layerId, cellX, cellY, terrain, cellX, cellY, terrain, write))
            outWrites.push_back(write);
    }

    // The neighbours keep the terrain they already carry; only the tile that
    // expresses it changes, because one of their peering slots now faces a
    // different terrain. A neighbour outside this terrain set is left alone.
    for (uint32 slot = 0; slot < TileMap::kPeeringSlotCount; ++slot)
    {
        const TileMap::PeeringSlotOffset offset = TileMap::kPeeringSlotOffsets[slot];
        int32 neighborX = 0;
        int32 neighborY = 0;
        if (!NeighborCell(cellX, cellY, offset, neighborX, neighborY))
            continue;
        const int16 neighborTerrain =
            TerrainAt(mapGuid, layerId, neighborX, neighborY, cellX, cellY, terrain);
        if (neighborTerrain < 0)
            continue;
        TerrainCellWrite write;
        if (ResolveCell(mapGuid, layerId, neighborX, neighborY, neighborTerrain, cellX, cellY,
                        terrain, write))
        {
            outWrites.push_back(write);
        }
    }
}

} // namespace Editor
} // namespace GameEngine
