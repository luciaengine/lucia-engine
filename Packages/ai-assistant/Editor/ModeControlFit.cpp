#include "ModeControlFit.h"

namespace GameEngine
{
ModeControlLayout ModeControlLayoutFor(const ModeControlFit& fit)
{
    const float others = fit.OthersPx;
    if (!fit.Stacked || fit.AvailablePx >= fit.WholeLabelPx + others)
        return {fit.WholeLabelPx, false};
    if (fit.AvailablePx >= fit.CompactLabelPx + others)
        return {fit.CompactLabelPx, true};
    return {std::nullopt, true};
}
} // namespace GameEngine
