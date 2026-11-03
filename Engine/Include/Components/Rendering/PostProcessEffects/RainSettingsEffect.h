#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{
// Attach to a PostProcessVolume to control the rain and water materials together.
struct RainSettingsEffect
{
    bool Enabled = true;
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 WaterDropSize = 1.0f;
    float32 FallingRainSizeOffset = 0.0f;
    float32 WaterRippleSpeed = 1.0f;
};
}
