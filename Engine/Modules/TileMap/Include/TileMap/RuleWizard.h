#pragma once

#include "AssetCore/Types.h"
#include "TileMap/TileRules.h"

namespace GameEngine
{
namespace TileMap
{

class TileSetAsset;

/** Which standard auto-tiling template the wizard writes out. */
enum class RuleTemplate : uint8
{
    /** 16 rules, one per corner configuration: the corner-matching layout. */
    Corners16 = 0,
    /** 47 rules, one per reachable side+corner configuration: the blob layout. */
    Blob47 = 1
};

/**
 * @brief What a generated group dresses: which terrain of which terrain set, and
 * which IntGrid value stands for it.
 */
struct RuleWizardRequest
{
    int16 TerrainSet = -1;
    int16 Terrain = -1;
    uint8 IntGridValue = 1;
    RuleTemplate Template = RuleTemplate::Corners16;
    /** Seed for the wizard's own tile picks; the terrain matcher's tie-break uses it. */
    uint32 Seed = 0;
};

/**
 * Builds a rule group that reproduces the terrain set's own matching from
 * IntGrid values: every configuration of "is the neighbour this terrain" becomes
 * one mutually exclusive pattern, and its output is the tile the terrain matcher
 * would have picked for that configuration.
 *
 * Rule ids are assigned from `rules.NextRuleId`, which is advanced, so the group
 * can be appended to an existing set without colliding.
 *
 * Returns a group with no rules when the request names no terrain set, no
 * terrain, or a terrain set the tileset does not have.
 */
TileRuleGroup GenerateTerrainRuleGroup(const TileSetAsset& tileSet, const RuleWizardRequest& request,
                                       TileRuleSet& rules);

} // namespace TileMap
} // namespace GameEngine
