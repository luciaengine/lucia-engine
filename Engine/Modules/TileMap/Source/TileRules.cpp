#include "TileMap/TileRules.h"

#include <algorithm>

namespace GameEngine
{
namespace TileMap
{

bool TileRule::IsWellFormed() const
{
    if (!IsValidRulePatternSize(PatternSize))
        return false;
    const size_t expected = static_cast<size_t>(PatternSize) * static_cast<size_t>(PatternSize);
    if (Pattern.size() != expected)
        return false;
    if (OutputMode == RuleOutputMode::Stamp)
        return Stamp.size() == expected;
    return !Tiles.empty();
}

int32 TileRuleSet::MaxPatternRadius() const
{
    int32 radius = 0;
    for (const TileRuleGroup& group : Groups)
    {
        for (const TileRule& rule : group.Rules)
        {
            if (rule.IsWellFormed())
                radius = std::max(radius, RulePatternRadius(rule.PatternSize));
        }
    }
    return radius;
}

} // namespace TileMap
} // namespace GameEngine
