#include "Includes/foliage_season.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput surface = DefaultSurfaceOutput();
    surface.opacity = float(dot(sIn.uv0, sIn.uv0) <= 1.0);
    float leafFraction = sIn.custom0.a;
    // Match the source materials: only leaves turn, and bark takes less snow.
    vec2 seasonMask = vec2(leafFraction, mix(0.6, 1.0, leafFraction));
    surface.baseColor = GE_ApplyFoliageSeason(sIn.custom0.rgb, sIn.normalWS, seasonMask,
                                            Light.uFoliageAutumnTint, Light.uFoliageSnow.x);
    surface.normalWS = sIn.normalWS;
    surface.roughness = 0.82;
    return surface;
}
