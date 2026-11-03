#include "TileMap/TerrainMatcher.h"
#include "TileMap/TileSetAsset.h"

#include <gtest/gtest.h>

#include <array>
#include <set>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

constexpr int16 kGrass = 0;
constexpr int16 kDirt = 1;
constexpr uint32 kAnySeed = 12345u;
constexpr const char* kTestPath = "TerrainMatcherTests.tileset";

// TileSetAsset is non-copyable, so the fixtures fill a caller-owned asset
// rather than returning one.
void InitTerrainSet(TileSetAsset& tileSet, TerrainMatchMode mode)
{
    TerrainSetDef set;
    set.Name = "Ground";
    set.MatchMode = mode;
    set.Terrains.push_back(TerrainDef{"Grass", 0xFF00FF00u});
    set.Terrains.push_back(TerrainDef{"Dirt", 0xFF2277AAu});
    tileSet.TerrainSets().push_back(std::move(set));
}

/** Appends a tile whose peering is spelled out slot by slot. */
TileSetTile& AddTile(TileSetAsset& tileSet, uint32 id, int16 center,
                     const std::array<int16, kPeeringSlotCount>& neighbors)
{
    TileSetTile tile;
    tile.Id = id;
    tile.Peering.TerrainSet = 0;
    tile.Peering.Center = center;
    tile.Peering.Neighbors = neighbors;
    tileSet.Tiles().push_back(std::move(tile));
    return tileSet.Tiles().back();
}

/**
 * The four corner slots of the clockwise slot order, in the bit order the corner
 * tests count in: TopLeft, TopRight, BottomRight, BottomLeft.
 */
constexpr std::array<uint32, 4> kCornerSlots{
    static_cast<uint32>(PeeringSlot::TopLeft), static_cast<uint32>(PeeringSlot::TopRight),
    static_cast<uint32>(PeeringSlot::BottomRight), static_cast<uint32>(PeeringSlot::BottomLeft)};

constexpr std::array<uint32, 4> kSideSlots{
    static_cast<uint32>(PeeringSlot::Top), static_cast<uint32>(PeeringSlot::Right),
    static_cast<uint32>(PeeringSlot::Bottom), static_cast<uint32>(PeeringSlot::Left)};

/** Corner terrains for combination `mask`: bit i set means corner i is dirt. */
std::array<int16, kPeeringSlotCount> CornerNeighbors(uint32 mask)
{
    std::array<int16, kPeeringSlotCount> neighbors{-1, -1, -1, -1, -1, -1, -1, -1};
    for (uint32 corner = 0; corner < kCornerSlots.size(); ++corner)
        neighbors[kCornerSlots[corner]] = (mask & (1u << corner)) != 0u ? kDirt : kGrass;
    return neighbors;
}

/** A 16-tile corner set: one tile per combination, ids 1..16. */
void InitCornerSet(TileSetAsset& tileSet)
{
    InitTerrainSet(tileSet, TerrainMatchMode::Corners);
    for (uint32 mask = 0; mask < 16u; ++mask)
        AddTile(tileSet, mask + 1u, kGrass, CornerNeighbors(mask));
}

} // namespace

TEST(TerrainMatcher, TheFullCornerSetResolvesEveryCornerConfiguration)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    for (uint32 mask = 0; mask < 16u; ++mask)
    {
        TerrainNeighborhood wanted;
        wanted.Center = kGrass;
        wanted.Neighbors = CornerNeighbors(mask);
        const TerrainMatchResult match = MatchTerrainTile(tileSet, 0, wanted, kAnySeed);
        ASSERT_TRUE(match.Matched) << "mask " << mask;
        EXPECT_EQ(match.TileId, mask + 1u) << "mask " << mask;
    }
}

TEST(TerrainMatcher, CornerMatchingIgnoresTheSideSlots)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    // Side annotations that contradict the request must not change the winner.
    for (TileSetTile& tile : tileSet.Tiles())
        for (const uint32 slot : kSideSlots)
            tile.Peering.Neighbors[slot] = kDirt;

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = CornerNeighbors(0b0101u);
    for (const uint32 slot : kSideSlots)
        wanted.Neighbors[slot] = kGrass;

    const TerrainMatchResult match = MatchTerrainTile(tileSet, 0, wanted, kAnySeed);
    ASSERT_TRUE(match.Matched);
    EXPECT_EQ(match.TileId, 0b0101u + 1u);
}

TEST(TerrainMatcher, SideMatchingIgnoresTheCornerSlots)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitTerrainSet(tileSet, TerrainMatchMode::Sides);
    std::array<int16, kPeeringSlotCount> sidesGrassCornersGrass{-1, -1, -1, -1, -1, -1, -1, -1};
    for (const uint32 slot : kSideSlots)
        sidesGrassCornersGrass[slot] = kGrass;
    for (const uint32 slot : kCornerSlots)
        sidesGrassCornersGrass[slot] = kGrass;
    std::array<int16, kPeeringSlotCount> sidesGrassCornersDirt = sidesGrassCornersGrass;
    for (const uint32 slot : kCornerSlots)
        sidesGrassCornersDirt[slot] = kDirt;

    // Tile 1 disagrees on every corner, tile 2 on one side. Under Sides matching
    // only the side disagreement counts, so tile 1 wins.
    AddTile(tileSet, 1, kGrass, sidesGrassCornersDirt);
    std::array<int16, kPeeringSlotCount> oneSideWrong = sidesGrassCornersGrass;
    oneSideWrong[static_cast<uint32>(PeeringSlot::Top)] = kDirt;
    AddTile(tileSet, 2, kGrass, oneSideWrong);

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = sidesGrassCornersGrass;
    const TerrainMatchResult match = MatchTerrainTile(tileSet, 0, wanted, kAnySeed);
    ASSERT_TRUE(match.Matched);
    EXPECT_EQ(match.TileId, 1u);
}

TEST(TerrainMatcher, MixedMatchingOutweighsCornersWithSides)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitTerrainSet(tileSet, TerrainMatchMode::CornersAndSides);

    std::array<int16, kPeeringSlotCount> allGrass{};
    allGrass.fill(kGrass);

    // Tile 1 agrees on all four sides and no corner; tile 2 is the mirror image.
    std::array<int16, kPeeringSlotCount> sidesOnly = allGrass;
    for (const uint32 slot : kCornerSlots)
        sidesOnly[slot] = kDirt;
    std::array<int16, kPeeringSlotCount> cornersOnly = allGrass;
    for (const uint32 slot : kSideSlots)
        cornersOnly[slot] = kDirt;
    AddTile(tileSet, 1, kGrass, sidesOnly);
    AddTile(tileSet, 2, kGrass, cornersOnly);

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = allGrass;
    const TerrainMatchResult match = MatchTerrainTile(tileSet, 0, wanted, kAnySeed);
    ASSERT_TRUE(match.Matched);
    EXPECT_EQ(match.TileId, 1u);

    // The same two tiles under corner matching invert the verdict, which is the
    // whole point of the mode carrying into the score.
    TileSetAsset cornerSet(GUID::Null(), kTestPath);
    InitTerrainSet(cornerSet, TerrainMatchMode::Corners);
    AddTile(cornerSet, 1, kGrass, sidesOnly);
    AddTile(cornerSet, 2, kGrass, cornersOnly);
    const TerrainMatchResult cornerMatch = MatchTerrainTile(cornerSet, 0, wanted, kAnySeed);
    ASSERT_TRUE(cornerMatch.Matched);
    EXPECT_EQ(cornerMatch.TileId, 2u);
}

TEST(TerrainMatcher, TheBestPartialWinsWhenNoTileFitsExactly)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    // Drop the exact answer for this configuration; the nearest neighbour, one
    // corner away, must take its place rather than an arbitrary survivor.
    constexpr uint32 kMissingMask = 0b1011u;
    auto& tiles = tileSet.Tiles();
    tiles.erase(tiles.begin() + kMissingMask);

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = CornerNeighbors(kMissingMask);
    const TerrainMatchResult match = MatchTerrainTile(tileSet, 0, wanted, kAnySeed);
    ASSERT_TRUE(match.Matched);

    const uint32 chosenMask = match.TileId - 1u;
    EXPECT_NE(chosenMask, kMissingMask);
    // One bit of Hamming distance: three of the four corners still agree.
    const uint32 difference = chosenMask ^ kMissingMask;
    EXPECT_EQ(difference & (difference - 1u), 0u) << "chosen mask " << chosenMask;
}

TEST(TerrainMatcher, AnUnrepresentedCenterTerrainIsAMiss)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    TerrainNeighborhood wanted;
    wanted.Center = kDirt; // every tile in the set is centred on grass
    wanted.Neighbors = CornerNeighbors(0u);
    const TerrainMatchResult match = MatchTerrainTile(tileSet, 0, wanted, kAnySeed);
    EXPECT_FALSE(match.Matched);
    EXPECT_EQ(match.TileId, 0u);
}

TEST(TerrainMatcher, AnUnknownTerrainSetIsAMiss)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = CornerNeighbors(0u);
    EXPECT_FALSE(MatchTerrainTile(tileSet, 1, wanted, kAnySeed).Matched);
    EXPECT_FALSE(MatchTerrainTile(tileSet, -1, wanted, kAnySeed).Matched);
}

TEST(TerrainMatcher, TilesFromAnotherTerrainSetAreNotCandidates)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    for (TileSetTile& tile : tileSet.Tiles())
        tile.Peering.TerrainSet = 1;

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = CornerNeighbors(0u);
    EXPECT_FALSE(MatchTerrainTile(tileSet, 0, wanted, kAnySeed).Matched);
}

TEST(TerrainMatcher, TheTieBreakIsDeterministicPerSeed)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitTerrainSet(tileSet, TerrainMatchMode::Corners);
    const std::array<int16, kPeeringSlotCount> neighbors = CornerNeighbors(0u);
    AddTile(tileSet, 1, kGrass, neighbors);
    AddTile(tileSet, 2, kGrass, neighbors);
    AddTile(tileSet, 3, kGrass, neighbors);

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = neighbors;

    for (uint32 seed = 0; seed < 64u; ++seed)
    {
        const TerrainMatchResult first = MatchTerrainTile(tileSet, 0, wanted, seed);
        ASSERT_TRUE(first.Matched);
        EXPECT_EQ(MatchTerrainTile(tileSet, 0, wanted, seed).TileId, first.TileId);
    }

    // Distinct seeds must actually reach distinct tiles, or the determinism
    // above would be satisfied by a constant.
    std::set<uint32> reached;
    for (uint32 seed = 0; seed < 256u; ++seed)
        reached.insert(MatchTerrainTile(tileSet, 0, wanted, seed).TileId);
    EXPECT_EQ(reached.size(), 3u);
}

TEST(TerrainMatcher, ProbabilityWeightsTheTieBreak)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitTerrainSet(tileSet, TerrainMatchMode::Corners);
    const std::array<int16, kPeeringSlotCount> neighbors = CornerNeighbors(0u);
    AddTile(tileSet, 1, kGrass, neighbors).Probability = 1.0f;
    AddTile(tileSet, 2, kGrass, neighbors).Probability = 9.0f;

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = neighbors;

    constexpr uint32 kTrials = 2000u;
    uint32 heavyCount = 0;
    for (uint32 seed = 0; seed < kTrials; ++seed)
    {
        if (MatchTerrainTile(tileSet, 0, wanted, seed).TileId == 2u)
            ++heavyCount;
    }
    // The 9:1 weighting has to dominate; the band is wide enough that only a
    // broken weighting, not sampling noise, can fail it.
    EXPECT_GT(heavyCount, kTrials * 4u / 5u);
    EXPECT_LT(heavyCount, kTrials);
}

TEST(TerrainMatcher, ZeroProbabilityTilesStillPaintWhenNothingElseFits)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitTerrainSet(tileSet, TerrainMatchMode::Corners);
    const std::array<int16, kPeeringSlotCount> neighbors = CornerNeighbors(0u);
    AddTile(tileSet, 7, kGrass, neighbors).Probability = 0.0f;

    TerrainNeighborhood wanted;
    wanted.Center = kGrass;
    wanted.Neighbors = neighbors;
    const TerrainMatchResult match = MatchTerrainTile(tileSet, 0, wanted, kAnySeed);
    ASSERT_TRUE(match.Matched);
    EXPECT_EQ(match.TileId, 7u);
}

TEST(TerrainMatcher, CellTerrainReadsTheTilesCenterAnnotation)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    EXPECT_EQ(TerrainOfCell(tileSet, MakeCell(1), 0), kGrass);
    // Orientation bits must not disturb the lookup: a flipped cell is the same tile.
    EXPECT_EQ(TerrainOfCell(tileSet, MakeCell(4, true, true, true), 0), kGrass);
    EXPECT_EQ(TerrainOfCell(tileSet, kEmptyCell, 0), -1);
    EXPECT_EQ(TerrainOfCell(tileSet, MakeCell(999), 0), -1);
    // The same tile queried against a terrain set it does not belong to.
    EXPECT_EQ(TerrainOfCell(tileSet, MakeCell(1), 1), -1);
}

TEST(TerrainMatcher, PeeringSlotOffsetsFollowTheClockwiseOrder)
{
    EXPECT_EQ(kPeeringSlotOffsets[static_cast<uint32>(PeeringSlot::Top)].Y, 1);
    EXPECT_EQ(kPeeringSlotOffsets[static_cast<uint32>(PeeringSlot::Top)].X, 0);
    EXPECT_EQ(kPeeringSlotOffsets[static_cast<uint32>(PeeringSlot::Bottom)].Y, -1);
    EXPECT_EQ(kPeeringSlotOffsets[static_cast<uint32>(PeeringSlot::Right)].X, 1);
    EXPECT_EQ(kPeeringSlotOffsets[static_cast<uint32>(PeeringSlot::Left)].X, -1);
    EXPECT_EQ(kPeeringSlotOffsets[static_cast<uint32>(PeeringSlot::TopLeft)].X, -1);
    EXPECT_EQ(kPeeringSlotOffsets[static_cast<uint32>(PeeringSlot::TopLeft)].Y, 1);

    for (const uint32 slot : kSideSlots)
        EXPECT_TRUE(IsSidePeeringSlot(slot)) << "slot " << slot;
    for (const uint32 slot : kCornerSlots)
        EXPECT_FALSE(IsSidePeeringSlot(slot)) << "slot " << slot;
}
