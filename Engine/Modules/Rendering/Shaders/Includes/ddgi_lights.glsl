// The packed light record of the DDGI trace kernels and the pure functions that read it.
// This file declares no bindings, so a test kernel can run its functions on authored records.

#ifndef GE_DDGI_LIGHTS_GLSL
#define GE_DDGI_LIGHTS_GLSL

// The field list of GE_LightPacked (light_packed_fields.glsl) over DDGI's own light buffer of world positions
// (DDGIProbeFeature.cpp), not the forward LightBuffer. Declared here: clustered_lighting.glsl also declares
// cluster and shadow bindings this ray-hit context does not use.
struct GE_DDGILightPacked
{
#include "light_packed_fields.glsl"
};

// meta.x of an emissive instance that DDGIProbeFeature publishes as a sphere-proxy light.
// shadowSlots.y of that record is the GPUInstance index of the emissive instance.
const uint GE_DDGI_LIGHT_TYPE_EMITTER_PROXY = 3u;

// The shadow ray of an emitter proxy goes to the proxy center, which can be inside the emitter mesh.
// Thus the ray needs its nearest hit, not the first hit that the traversal finds.
bool GE_DDGIShadowRayNeedsNearestHit(GE_DDGILightPacked lp)
{
    return lp.meta.x == GE_DDGI_LIGHT_TYPE_EMITTER_PROXY;
}

// True when the hit of a shadow ray on instance `hitInstance` hides the light.
// The mesh of an emitter does not hide its own proxy. Each other instance hides it.
bool GE_DDGIShadowHitHidesLight(GE_DDGILightPacked lp, int hitInstance)
{
    return lp.meta.x != GE_DDGI_LIGHT_TYPE_EMITTER_PROXY || hitInstance != lp.shadowSlots.y;
}

#endif // GE_DDGI_LIGHTS_GLSL
