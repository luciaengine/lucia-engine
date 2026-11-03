#include "TileMap/RuleWizard.h"

#include "TileMap/TerrainMatcher.h"
#include "TileMap/TerrainPeering.h"
#include "TileMap/TileSetAsset.h"

#include <array>
#include <set>
#include <string>

namespace GameEngine
{
namespace TileMap
{
namespace
{

constexpr int32 kWizardPatternSize = 3;

/**
 * The four corner slots in the bit order the templates count in: TopLeft,
 * TopRight, BottomRight, BottomLeft — clockwise, like the slot order itself.
 */
constexpr std::array<uint32, 4> kCornerSlots{
    static_cast<uint32>(PeeringSlot::TopLeft), static_cast<uint32>(PeeringSlot::TopRight),
    static_cast<uint32>(PeeringSlot::BottomRight), static_cast<uint32>(PeeringSlot::BottomLeft)};

/** Pattern index of the cell a peering slot points at, in a 3x3 pattern. */
size_t PatternIndexOfSlot(uint32 slot)
{
    const PeeringSlotOffset offset = kPeeringSlotOffsets[slot];
    // Pattern row 0 is the topmost row, while cell Y grows upward.
    const int32 px = offset.X + 1;
    const int32 py = 1 - offset.Y;
    return static_cast<size_t>(py) * kWizardPatternSize + static_cast<size_t>(px);
}

constexpr size_t kPatternCentreIndex = (kWizardPatternSize * kWizardPatternSize) / 2;

/** The sides flanking a corner slot, in the clockwise slot order. */
std::array<uint32, 2> AdjacentSides(uint32 cornerSlot)
{
    return {(cornerSlot + 1u) % kPeeringSlotCount, (cornerSlot + kPeeringSlotCount - 1u) %
                                                       kPeeringSlotCount};
}

/**
 * A corner only carries information when both of its sides are filled: with a
 * side open, the corner tile is the side tile's business. Clearing those bits is
 * what collapses 256 configurations onto the blob set's 47.
 */
uint32 CanonicalBlobMask(uint32 mask)
{
    uint32 canonical = mask;
    for (const uint32 corner : kCornerSlots)
    {
        const std::array<uint32, 2> sides = AdjacentSides(corner);
        const bool bothSides = (mask & (1u << sides[0])) != 0u && (mask & (1u << sides[1])) != 0u;
        if (!bothSides)
            canonical &= ~(1u << corner);
    }
    return canonical;
}

void SetPatternCell(TileRule& rule, size_t index, bool require, uint8 value)
{
    rule.Pattern[index].Op = require ? RulePatternOp::RequireValue : RulePatternOp::ForbidValue;
    rule.Pattern[index].Value = value;
}

TileRule MakeBaseRule(uint32 id, std::string name, uint8 value)
{
    TileRule rule;
    rule.Id = id;
    rule.Name = std::move(name);
    rule.PatternSize = kWizardPatternSize;
    rule.Pattern.assign(kWizardPatternSize * kWizardPatternSize, RulePatternCell{});
    rule.Pattern[kPatternCentreIndex].Op = RulePatternOp::RequireValue;
    rule.Pattern[kPatternCentreIndex].Value = value;
    return rule;
}

/** Emits one rule for `mask`, or nothing when the terrain set cannot express it. */
void EmitRule(const TileSetAsset& tileSet, const RuleWizardRequest& request, TileRuleSet& rules,
              uint32 mask, const std::string& name, bool cornersOnly, TileRuleGroup& outGroup)
{
    TerrainNeighborhood wanted;
    wanted.Center = request.Terrain;

    TileRule rule = MakeBaseRule(rules.NextRuleId, name, request.IntGridValue);
    for (uint32 slot = 0; slot < kPeeringSlotCount; ++slot)
    {
        const bool corner = !IsSidePeeringSlot(slot);
        if (cornersOnly && !corner)
            continue; // sides are don't-care in the corner template

        const bool set = (mask & (1u << slot)) != 0u;
        if (!cornersOnly && corner)
        {
            const std::array<uint32, 2> sides = AdjacentSides(slot);
            const bool bothSides =
                (mask & (1u << sides[0])) != 0u && (mask & (1u << sides[1])) != 0u;
            if (!bothSides)
                continue; // the canonical form leaves this corner open
        }

        SetPatternCell(rule, PatternIndexOfSlot(slot), set, request.IntGridValue);
        wanted.Neighbors[slot] = set ? request.Terrain : static_cast<int16>(-1);
    }

    const TerrainMatchResult match =
        MatchTerrainTile(tileSet, request.TerrainSet, wanted, request.Seed + mask);
    if (!match.Matched)
        return;

    rule.Tiles.push_back(RuleTileOutput{MakeCell(match.TileId), 1.0f});
    outGroup.Rules.push_back(std::move(rule));
    ++rules.NextRuleId;
}

} // namespace

TileRuleGroup GenerateTerrainRuleGroup(const TileSetAsset& tileSet, const RuleWizardRequest& request,
                                       TileRuleSet& rules)
{
    TileRuleGroup group;
    group.Id = rules.NextGroupId;
    if (request.TerrainSet < 0 || request.Terrain < 0 || request.IntGridValue == 0 ||
        static_cast<size_t>(request.TerrainSet) >= tileSet.TerrainSets().size())
    {
        return group;
    }

    const TerrainSetDef& terrainSet = tileSet.TerrainSets()[static_cast<size_t>(request.TerrainSet)];
    const std::string terrainName =
        static_cast<size_t>(request.Terrain) < terrainSet.Terrains.size()
            ? terrainSet.Terrains[static_cast<size_t>(request.Terrain)].Name
            : std::string("Terrain");
    group.Name = terrainName + (request.Template == RuleTemplate::Blob47 ? " blob" : " corners");

    if (request.Template == RuleTemplate::Corners16)
    {
        for (uint32 combination = 0; combination < 16u; ++combination)
        {
            uint32 mask = 0;
            for (uint32 corner = 0; corner < kCornerSlots.size(); ++corner)
            {
                if ((combination & (1u << corner)) != 0u)
                    mask |= 1u << kCornerSlots[corner];
            }
            EmitRule(tileSet, request, rules, mask, "Corners " + std::to_string(combination), true,
                     group);
        }
    }
    else
    {
        std::set<uint32> emitted;
        for (uint32 mask = 0; mask < 256u; ++mask)
        {
            const uint32 canonical = CanonicalBlobMask(mask);
            if (!emitted.insert(canonical).second)
                continue;
            EmitRule(tileSet, request, rules, canonical, "Blob " + std::to_string(canonical), false,
                     group);
        }
    }

    ++rules.NextGroupId;
    return group;
}

} // namespace TileMap
} // namespace GameEngine
