#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{

/// Opt in to the model's reduced bone tier below the projected coverage threshold.
/// Imported children inherit their model-instance owner's settings.
struct BoneLOD
{
    float32 SwitchCoverage = 0.05f;
    // Keep all bones during actions that need arbitrary pose or attachment data.
    bool FullDetailOverride = false;
};

} // namespace GameEngine::Components
