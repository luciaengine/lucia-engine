#pragma once

#include "AssetCore/Types.h"
#include "TileMap/TileCellEncoding.h"

#include <string>
#include <vector>

namespace GameEngine
{
namespace TileMap
{

/** Pattern sizes a rule may use: odd, so the matched cell is the centre. */
inline constexpr int32 kMinRulePatternSize = 1;
inline constexpr int32 kMaxRulePatternSize = 5;

/** True for the sizes a rule pattern may take (odd, within the supported range). */
constexpr bool IsValidRulePatternSize(int32 size)
{
    return size >= kMinRulePatternSize && size <= kMaxRulePatternSize && (size % 2) == 1;
}

/** The radius of a pattern: how far from the centre cell it reaches. */
constexpr int32 RulePatternRadius(int32 size) { return size / 2; }

/**
 * @brief What one pattern cell demands of the IntGrid value under it.
 *
 * RequireValue/ForbidValue read Value; the value 0 is a legitimate subject —
 * "require 0" is "must be blank", "forbid 0" is "must be anything".
 */
enum class RulePatternOp : uint8
{
    DontCare = 0,
    RequireValue = 1,
    ForbidValue = 2,
    RequireAny = 3 // any non-zero value
};

struct RulePatternCell
{
    RulePatternOp Op = RulePatternOp::DontCare;
    uint8 Value = 0;

    bool operator==(const RulePatternCell&) const = default;
};

/**
 * @brief One candidate output of a rule and its share of the weighted pick.
 *
 * The cell carries the tile id plus any orientation bits the author baked in;
 * mirrored matches XOR their own flip bits on top.
 */
struct RuleTileOutput
{
    TileCell Cell = kEmptyCell;
    float32 Weight = 1.0f;

    bool operator==(const RuleTileOutput&) const = default;
};

/** Whether a rule paints its centre cell or stamps its whole footprint. */
enum class RuleOutputMode : uint8
{
    WeightedTiles = 0,
    Stamp = 1
};

/**
 * @brief One pattern rule: an NxN test over IntGrid values and what to paint
 * where it holds.
 *
 * Pattern cells are row-major from the top-left, so row 0 is the topmost row
 * (+Y) — the same reading order as the editor's pattern grid and as a tileset
 * page. Stamp cells use the same layout and the same footprint; an empty stamp
 * cell leaves the map cell alone rather than clearing it.
 */
struct TileRule
{
    uint32 Id = 0;
    std::string Name;
    bool Enabled = true;

    int32 PatternSize = 3;
    std::vector<RulePatternCell> Pattern; // PatternSize² entries, row-major

    RuleOutputMode OutputMode = RuleOutputMode::WeightedTiles;
    std::vector<RuleTileOutput> Tiles;    // WeightedTiles
    std::vector<TileCell> Stamp;          // Stamp: PatternSize² entries, row-major

    /** Probability the rule applies where it matches; 0 never fires, 1 always. */
    float32 Chance = 1.0f;
    /** Stop evaluating this cell once this rule has matched. */
    bool BreakOnMatch = true;

    /** Also try the pattern mirrored on X / on Y, flipping the output to suit. */
    bool MirrorX = false;
    bool MirrorY = false;

    /** Restrict the rule to every Nth column/row, phased by the offset. */
    int32 ModuloX = 1;
    int32 ModuloY = 1;
    int32 OffsetX = 0;
    int32 OffsetY = 0;

    /** True when the pattern vector matches the declared, supported size. */
    bool IsWellFormed() const;
};

/** @brief A named, ordered bundle of rules; groups are themselves ordered. */
struct TileRuleGroup
{
    uint32 Id = 0;
    std::string Name;
    bool Enabled = true;
    std::vector<TileRule> Rules;
};

/**
 * @brief The auto-tiling program attached to one Tiles layer: which IntGrid
 * layer it reads and the ordered groups that dress it.
 *
 * Ids are stable across reorder so the editor can address a rule while the
 * lists move under it, and the seed is persisted so a map re-dresses to the
 * same art on any machine.
 */
struct TileRuleSet
{
    uint32 SourceLayerId = 0; // IntGrid layer in the same map; 0 = unbound
    uint32 Seed = 0;
    std::vector<TileRuleGroup> Groups;

    uint32 NextGroupId = 1;
    uint32 NextRuleId = 1;

    bool IsEmpty() const { return Groups.empty(); }

    /** The largest pattern radius in the set: the dilation an incremental pass needs. */
    int32 MaxPatternRadius() const;
};

} // namespace TileMap
} // namespace GameEngine
