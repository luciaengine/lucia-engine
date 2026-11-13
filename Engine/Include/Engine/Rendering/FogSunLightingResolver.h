#pragma once

#include "Mathematics/Vector3.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Types/Color.h"

namespace GameEngine::Engine::Renderer {

class RenderServices;

/// The sun that fog and clouds scatter: the unit direction toward the sun, its scene-linear color
/// and its intensity.
struct ResolvedFogSun
{
    Mathematics::Vector3 Direction{0.35f, -0.65f, 0.68f};
    ColorLinear Color{1.0f, 0.86f, 0.62f};
    float Intensity = 1.0f;
};

/// Resolves the fog sun from the primary directional light, then from the sky sun, then from
/// @p fallback. Only a tracked sun reads the light and the sky. The intensity is scaled by
/// @p sunIntensityScale.
ResolvedFogSun ResolveFogSunLighting(
    RenderServices& services,
    ::GameEngine::Rendering::ViewId viewId,
    bool trackDirectionalLight,
    float sunIntensityScale,
    const ResolvedFogSun& fallback);

} // namespace GameEngine::Engine::Renderer
