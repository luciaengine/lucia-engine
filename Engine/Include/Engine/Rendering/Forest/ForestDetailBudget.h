#pragma once

#include "Types/Types.h"

#include <optional>

namespace GameEngine::Engine::Renderer
{

/// Visible pages inside the requested detail distance of one view. Prefetch pages are excluded.
struct ForestDetailBudget
{
    uint32 Eligible = 0;
    uint32 Selected = 0;
    uint32 Resident = 0;
    /// Distance from the eye to the nearest page bounds excluded by the detail slot cap, in meters.
    std::optional<double> NearestOmitted;
};

} // namespace GameEngine::Engine::Renderer
