#include "TileMap/TerrainMatcher.h"

#include "TileMap/TileSetAsset.h"

#include <algorithm>

namespace GameEngine
{
namespace TileMap
{
namespace
{

// A wrong side edge cuts a visible seam between two terrains; a wrong corner
// only blemishes one diagonal. In the mixed mode the sides therefore outrank
// the corners, and no combination of corners can outvote a matching side pair.
constexpr int32 kSideSlotWeight = 3;
constexpr int32 kCornerSlotWeight = 1;

int32 SlotWeight(uint32 slot, TerrainMatchMode mode)
{
    const bool side = IsSidePeeringSlot(slot);
    switch (mode)
    {
    case TerrainMatchMode::Corners:
        return side ? 0 : kCornerSlotWeight;
    case TerrainMatchMode::Sides:
        return side ? kSideSlotWeight : 0;
    case TerrainMatchMode::CornersAndSides:
        break;
    }
    return side ? kSideSlotWeight : kCornerSlotWeight;
}

int32 ScoreTile(const TerrainPeering& peering, const TerrainNeighborhood& wanted,
                TerrainMatchMode mode)
{
    int32 score = 0;
    for (uint32 slot = 0; slot < kPeeringSlotCount; ++slot)
    {
        if (peering.Neighbors[slot] == wanted.Neighbors[slot])
            score += SlotWeight(slot, mode);
    }
    return score;
}

/** splitmix32: the tie-break needs a well-spread, reproducible value per seed. */
uint32 HashSeed(uint32 seed)
{
    uint32 z = seed + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

} // namespace

TerrainMatchResult MatchTerrainTile(const TileSetAsset& tileSet, int16 terrainSet,
                                    const TerrainNeighborhood& wanted, uint32 seed)
{
    TerrainMatchResult result;
    if (terrainSet < 0 || static_cast<size_t>(terrainSet) >= tileSet.TerrainSets().size())
        return result;

    const TerrainMatchMode mode = tileSet.TerrainSets()[static_cast<size_t>(terrainSet)].MatchMode;

    // Two passes over the tiles rather than a candidate vector: the winning
    // score has to be known before the weighted pick can be normalized, and a
    // tileset is small enough that walking it twice beats allocating per cell.
    int32 bestScore = 0;
    bool haveCandidate = false;
    float32 totalWeight = 0.0f;
    for (const TileSetTile& tile : tileSet.Tiles())
    {
        if (tile.Peering.TerrainSet != terrainSet || tile.Peering.Center != wanted.Center)
            continue;
        const int32 score = ScoreTile(tile.Peering, wanted, mode);
        if (!haveCandidate || score > bestScore)
        {
            haveCandidate = true;
            bestScore = score;
            totalWeight = 0.0f;
        }
        if (score == bestScore)
            totalWeight += std::max(0.0f, tile.Probability);
    }
    if (!haveCandidate)
        return result;

    constexpr float32 kHashDenominator = 4294967296.0f;
    const float32 pick =
        (static_cast<float32>(HashSeed(seed)) / kHashDenominator) * totalWeight;
    float32 running = 0.0f;
    for (const TileSetTile& tile : tileSet.Tiles())
    {
        if (tile.Peering.TerrainSet != terrainSet || tile.Peering.Center != wanted.Center)
            continue;
        if (ScoreTile(tile.Peering, wanted, mode) != bestScore)
            continue;
        result.TileId = tile.Id;
        result.Matched = true;
        running += std::max(0.0f, tile.Probability);
        if (pick < running)
            return result;
    }
    // All-zero probabilities leave `running` at 0 and no early return; the last
    // best-scoring tile stands in, so a set authored without probabilities at
    // all still paints.
    return result;
}

int16 TerrainOfCell(const TileSetAsset& tileSet, TileCell cell, int16 terrainSet)
{
    if (CellIsEmpty(cell))
        return -1;
    const TileSetTile* tile = tileSet.FindTile(CellTileId(cell));
    if (!tile || tile->Peering.TerrainSet != terrainSet)
        return -1;
    return tile->Peering.Center;
}

} // namespace TileMap
} // namespace GameEngine
