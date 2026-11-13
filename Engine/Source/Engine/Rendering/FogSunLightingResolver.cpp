#include "Engine/Rendering/FogSunLightingResolver.h"

#include "Components/Rendering/Light.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include <algorithm>
#include <span>

namespace GameEngine::Engine::Renderer {

ResolvedFogSun ResolveFogSunLighting(
    RenderServices& services,
    ::GameEngine::Rendering::ViewId viewId,
    bool trackDirectionalLight,
    float sunIntensityScale,
    const ResolvedFogSun& fallback)
{
    ResolvedFogSun sun = fallback;

    bool foundDirectional = false;
    if (trackDirectionalLight)
    {
        const ViewDesc* view = services.Views().FindViewDesc(viewId);
        const auto lights = view ? services.GetWorldLights(view->worldId) : std::span<const ExtractedLight>{};
        // Fog tracks THE primary directional (the light the world shades
        // with — SelectPrimaryDirectional), never a secondary one. If the
        // primary is non-emitting the sky-anchor fallback below applies;
        // scanning on to a weaker directional would aim the fog sun at a
        // light the surface shading is not using. "Non-emitting" is what it
        // delivers, not its intensity alone: a sky-driven sun at night with
        // the moon hidden is black at its authored intensity.
        const ExtractedLight* light = SelectLitPrimaryDirectional(lights);
        if (light)
        {
            sun.Direction =
                Mathematics::Vector3(-light->directionWS[0], -light->directionWS[1], -light->directionWS[2]);
            sun.Color = ColorLinear(
                std::max(light->color[0], 0.0f), std::max(light->color[1], 0.0f), std::max(light->color[2], 0.0f));
            sun.Intensity = std::max(light->intensity, 0.0f);
            foundDirectional = true;
        }
    }

    if (auto* skyFeature = services.GetFeature<SkyRenderFeature>(); skyFeature && skyFeature->HasActiveSettings())
    {
        const auto& sky = skyFeature->GetSettings();
        if (trackDirectionalLight && !foundDirectional)
        {
            sun.Direction = sky.primarySunDir;
            // Ground level: with no directional light to read the sky is the only source, and fog
            // sits under the atmosphere, so it takes the extinguished colour rather than the
            // above-atmosphere one.
            sun.Color = ColorLinear(std::max(sky.primarySunGroundColor.r, 0.0f),
                                    std::max(sky.primarySunGroundColor.g, 0.0f),
                                    std::max(sky.primarySunGroundColor.b, 0.0f));
            sun.Intensity = std::max(sky.primarySunIntensity * 0.1f, 0.0f);
        }
        // A tracked light needs nothing from the sky: its colour already IS the ground-level sun
        // colour whenever the sky drives it, so scaling it by the source's chromaticity would apply
        // the same hue twice — the moon's at night, an authored sun tint squared.
    }

    sun.Direction = sun.Direction.NormalizeOrZero();
    sun.Intensity *= std::max(sunIntensityScale, 0.0f);
    return sun;
}

} // namespace GameEngine::Engine::Renderer
