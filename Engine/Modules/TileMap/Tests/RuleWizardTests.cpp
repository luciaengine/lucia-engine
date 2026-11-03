#include "TileMap/RuleWizard.h"

#include "TileMap/TerrainMatcher.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileRuleEvaluator.h"
#include "TileMap/TileSetAsset.h"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

constexpr int16 kGround = 0;
constexpr uint8 kGroundValue = 1;
constexpr uint32 kWizardSeed = 7u;
constexpr const char* kTestPath = "RuleWizardTests.tileset";

/** Corner slots in the bit order the corner template counts in. */
constexpr std::array<uint32, 4> kCornerSlots{
    static_cast<uint32>(PeeringSlot::TopLeft), static_cast<uint32>(PeeringSlot::TopRight),
    static_cast<uint32>(PeeringSlot::BottomRight), static_cast<uint32>(PeeringSlot::BottomLeft)};

/**
 * Neighbour terrains for corner combination `mask`: a set bit is a neighbour
 * that carries the terrain, a clear bit a neighbour that carries none — which is
 * what an unpainted cell reports, and therefore what an IntGrid gap means.
 */
std::array<int16, kPeeringSlotCount> CornerNeighbors(uint32 mask)
{
    std::array<int16, kPeeringSlotCount> neighbors{-1, -1, -1, -1, -1, -1, -1, -1};
    for (uint32 corner = 0; corner < kCornerSlots.size(); ++corner)
    {
        if ((mask & (1u << corner)) != 0u)
            neighbors[kCornerSlots[corner]] = kGround;
    }
    return neighbors;
}

/** The slot-indexed mask the wizard seeds its tile pick with, for a combination. */
uint32 SlotMask(uint32 combination)
{
    uint32 mask = 0;
    for (uint32 corner = 0; corner < kCornerSlots.size(); ++corner)
    {
        if ((combination & (1u << corner)) != 0u)
            mask |= 1u << kCornerSlots[corner];
    }
    return mask;
}

/** The phase-4 corner set: one tile per corner configuration, ids 1..16. */
void InitCornerSet(TileSetAsset& tileSet)
{
    TerrainSetDef set;
    set.Name = "Ground";
    set.MatchMode = TerrainMatchMode::Corners;
    set.Terrains.push_back(TerrainDef{"Ground", 0xFF00FF00u});
    tileSet.TerrainSets().push_back(std::move(set));

    for (uint32 mask = 0; mask < 16u; ++mask)
    {
        TileSetTile tile;
        tile.Id = mask + 1u;
        tile.Peering.TerrainSet = 0;
        tile.Peering.Center = kGround;
        tile.Peering.Neighbors = CornerNeighbors(mask);
        tileSet.Tiles().push_back(std::move(tile));
    }
}

RuleWizardRequest CornerRequest()
{
    RuleWizardRequest request;
    request.TerrainSet = 0;
    request.Terrain = kGround;
    request.IntGridValue = kGroundValue;
    request.Template = RuleTemplate::Corners16;
    request.Seed = kWizardSeed;
    return request;
}

} // namespace

TEST(RuleWizard, TheCornerTemplateEmitsOneRulePerConfiguration)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);

    TileRuleSet rules;
    const TileRuleGroup group = GenerateTerrainRuleGroup(tileSet, CornerRequest(), rules);

    EXPECT_EQ(group.Rules.size(), 16u);
    EXPECT_EQ(group.Name, "Ground corners");
    std::set<uint32> ids;
    std::set<uint32> tiles;
    for (const TileRule& rule : group.Rules)
    {
        EXPECT_TRUE(rule.IsWellFormed());
        EXPECT_EQ(rule.PatternSize, 3);
        ASSERT_EQ(rule.Tiles.size(), 1u);
        ids.insert(rule.Id);
        tiles.insert(CellTileId(rule.Tiles[0].Cell));
    }
    EXPECT_EQ(ids.size(), 16u);
    // Every tile of the set is reachable, or the wizard collapsed configurations.
    EXPECT_EQ(tiles.size(), 16u);
    EXPECT_EQ(rules.NextRuleId, 17u);
}

TEST(RuleWizard, TheCornerTemplateDressesEveryConfigurationLikeTheTerrainMatcher)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);

    TileMapData map;
    const uint32 sourceId = map.AddLayer("Semantics", TileMapLayerType::IntGrid);
    const uint32 artId = map.AddLayer("Art", TileMapLayerType::Tiles);
    TileMapLayerData& art = *map.FindLayer(artId);
    art.Rules.SourceLayerId = sourceId;
    art.Rules.Seed = kWizardSeed;
    TileRuleGroup group = GenerateTerrainRuleGroup(tileSet, CornerRequest(), art.Rules);
    art.Rules.Groups.push_back(std::move(group));

    // One test cell per configuration, spaced far enough apart that neighbouring
    // configurations cannot bleed into each other's 3x3 pattern.
    constexpr int32 kSpacing = 8;
    TileMapLayerData& source = *map.FindLayer(sourceId);
    for (uint32 mask = 0; mask < 16u; ++mask)
    {
        const int32 cellX = static_cast<int32>(mask) * kSpacing;
        source.SetIntGridValue(cellX, 0, kGroundValue);
        const std::array<int16, kPeeringSlotCount> neighbors = CornerNeighbors(mask);
        for (uint32 slot = 0; slot < kPeeringSlotCount; ++slot)
        {
            if (neighbors[slot] < 0)
                continue;
            const PeeringSlotOffset offset = kPeeringSlotOffsets[slot];
            source.SetIntGridValue(cellX + offset.X, offset.Y, kGroundValue);
        }
    }

    std::vector<RuleCellChange> changed;
    ASSERT_TRUE(EvaluateRules(map, artId, changed));

    for (uint32 mask = 0; mask < 16u; ++mask)
    {
        TerrainNeighborhood wanted;
        wanted.Center = kGround;
        wanted.Neighbors = CornerNeighbors(mask);
        const TerrainMatchResult expected =
            MatchTerrainTile(tileSet, 0, wanted, kWizardSeed + SlotMask(mask));
        ASSERT_TRUE(expected.Matched) << "mask " << mask;
        EXPECT_EQ(CellTileId(art.GetCell(static_cast<int32>(mask) * kSpacing, 0)), expected.TileId)
            << "mask " << mask;
    }
}

TEST(RuleWizard, TheBlobTemplateCollapsesTo47Configurations)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);
    tileSet.TerrainSets()[0].MatchMode = TerrainMatchMode::CornersAndSides;

    TileRuleSet rules;
    RuleWizardRequest request = CornerRequest();
    request.Template = RuleTemplate::Blob47;
    const TileRuleGroup group = GenerateTerrainRuleGroup(tileSet, request, rules);

    EXPECT_EQ(group.Rules.size(), 47u);
    EXPECT_EQ(group.Name, "Ground blob");
}

TEST(RuleWizard, AnUnknownTerrainSetYieldsNoRules)
{
    TileSetAsset tileSet(GUID::Null(), kTestPath);
    InitCornerSet(tileSet);

    TileRuleSet rules;
    RuleWizardRequest request = CornerRequest();
    request.TerrainSet = 3;
    EXPECT_TRUE(GenerateTerrainRuleGroup(tileSet, request, rules).Rules.empty());

    request = CornerRequest();
    request.IntGridValue = 0;
    EXPECT_TRUE(GenerateTerrainRuleGroup(tileSet, request, rules).Rules.empty());
    EXPECT_EQ(rules.NextRuleId, 1u);
}
