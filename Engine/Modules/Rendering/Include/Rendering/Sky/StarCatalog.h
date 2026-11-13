#pragma once

#include "Mathematics/Vector4.h"

namespace GameEngine::Rendering
{

/// One night-sky star as sky_stars_billboard.vert reads it (std430, 32 bytes).
struct StarInstanceGPU
{
    /// xyz: unit direction in world space; w: visual (V) magnitude.
    Mathematics::Vector4 dirAndMagnitude;
    /// xyz: linear Rec.709 colour of the star's blackbody, luminance 1; w: twinkle seed in [0, 1).
    Mathematics::Vector4 colorAndSeed;
};
static_assert(sizeof(StarInstanceGPU) == 32, "StarInstanceGPU must match the std430 star record");

} // namespace GameEngine::Rendering
