#pragma once

#include "Components/Rendering/LightPhotometry.h"

#include <cmath>

namespace GameEngine::Rendering
{

// Absolute EV100 that maps the engine's 203-nit reference white to a linear multiplier of 1.
inline const float kNeutralExposureEV = std::log2(Components::kReferenceWhiteNits);

// Absolute EV100 to the linear scene multiplier consumed by the tonemap.
inline float EvToLinearExposure(float ev100)
{
    return std::exp2(kNeutralExposureEV - ev100);
}

// A linear scene multiplier to its absolute EV100.
inline float LinearExposureToEv(float scale)
{
    return kNeutralExposureEV - std::log2(scale > 1e-8f ? scale : 1e-8f);
}

} // namespace GameEngine::Rendering
