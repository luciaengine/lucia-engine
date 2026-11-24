// The light a surface sends toward the viewer for one light. Every forward lighting path (the
// primary and secondary directionals, the clustered point, spot and area lights) asks this, so a
// surface whose response is not the standard BRDF answers here and the BRDF stays one model.
// Six-way lit particles answer from their baked response maps; every other surface is StandardPBR.

#ifndef GE_SURFACE_LIGHT_RESPONSE_GLSL
#define GE_SURFACE_LIGHT_RESPONSE_GLSL

#include "standard_pbr.glsl" // GE_EvaluateStandardPBR + GE_PI
#include "microshadowing.glsl"
#ifdef GE_USER_PARTICLE_LIT
#include "particle_six_way.glsl" // GE_SixWayResponse
#endif

// Radiance toward V per unit of light radiance arriving from L. V is the view direction and L the
// surface-to-light direction, both unit length in world space.
vec3 GE_EvaluateSurfaceLight(SurfaceOutput so, vec3 V, vec3 L)
{
#ifdef GE_USER_PARTICLE_LIT
    // The response maps hold the light each signed axis of the particle's basis lets through.
    return so.baseColor * GE_SixWayResponse(so.particlePositive, so.particleNegative,
                                           transpose(so.particleBasis) * L) / GE_PI;
#elif defined(GE_DIFFUSE_TRANSMISSION_ENABLED)
    // A thin surface answers from both faces: the near face's reflection, and the light through it
    // from a light behind it.
    return GE_EvaluateStandardPBR(so, V, L)
         + GE_DiffuseTransmissionFarFace(so.diffuseTransmissionAlbedo * (1.0 - so.metallic),
                                         so.diffuseTransmission, dot(so.normalWS, L));
#else
    return GE_EvaluateStandardPBR(so, V, L);
#endif
}

vec3 GE_EvaluateDirectionalSurfaceLight(SurfaceOutput so, vec3 V, vec3 L)
{
    vec3 response = GE_EvaluateSurfaceLight(so, V, L);
    if (so.microshadowStrength <= 0.0 || so.ao >= 1.0)
        return response;

    // Back lighting through thin surfaces uses the transmission model.
    float NdotL = dot(so.normalWS, L);
    if (NdotL <= 0.0)
        return response;

    float visibility = GE_MicroshadowVisibility(so.ao, NdotL);
    return response * mix(1.0, visibility, clamp(so.microshadowStrength, 0.0, 1.0));
}

#endif
