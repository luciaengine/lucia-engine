// Standard PBR with a per-instance baked directional visibility texture.
// Requires UV1; ordinary materials and meshes without UV1 remain unaffected.
// Named slots reserve 0..5 for base PBR; the remaining two hold shadow and HDR GI.
// @texture albedoMap srgb
// @texture normalMap linear
// @texture metallicRoughnessMap linear
// @texture emissiveMap srgb
// @texture aoMap linear
// @texture coatNormalMap linear
// @texture shadowLightmapMap linear
// @texture indirectLightmapMap linear


// The generated material opts into the shared material-lighting hook.
#ifdef GE_USER_CUSTOM_LIGHTING
vec4 ge_BakedIrradiance = vec4(0.0);
vec4 ge_ShadowLightmap = vec4(1.0, 0.0, 0.0, 0.0);
vec3 ge_ShadowLightmapDirection = vec3(0.0);
float ge_BakedDiffuseWeight = 1.0;
#endif

#define EvaluateSurface GE_EvaluateShadowLightmapBase
#include "Surfaces/standard_pbr.glsl"
#undef EvaluateSurface

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = GE_EvaluateShadowLightmapBase(sIn);
#if defined(HAS_UV1) && defined(GE_USER_CUSTOM_LIGHTING)
    // Explicit LOD 0 avoids atlas-chart bleeding from generated mipmaps.
    // Bake padding supplies the bilinear gutter; do not apply material tiling.
    ge_ShadowLightmap = vec4(textureLod(GE_USER_TEXTURE(shadowLightmapMap), sIn.uv1, 0.0).r,
                           Mat.uUser0.xyz);
    ge_ShadowLightmapDirection = Mat.uUser1.xyz;
    if (Mat.uUser0.w > 0.5)
        ge_BakedIrradiance = vec4(textureLod(GE_USER_TEXTURE(indirectLightmapMap), sIn.uv1, 0.0).rgb, 1.0);
#endif
#ifdef GE_USER_CUSTOM_LIGHTING
    ge_BakedDiffuseWeight = 1.0 - o.metallic;
#endif
    return o;
}

#ifdef GE_USER_CUSTOM_LIGHTING
// Adapted directional Replace/Overlay equations; attribution travels with
// this package in Licenses/Godot-MIT.txt. A rotated sun invalidates the mask.
float GE_SurfaceDirectionalVisibility(float realtime, float depth, vec3 directionToLight)
{
    int mode = int(ge_ShadowLightmap.y + 0.5);
    if (!ge_SurfaceReceivesShadows || mode == 0 ||
        dot(ge_ShadowLightmapDirection, directionToLight) < 0.99999)
        return realtime;
    float baked = clamp(ge_ShadowLightmap.x, 0.0, 1.0);
    if (mode == 3) return baked;
    float fade = ge_ShadowLightmap.w > ge_ShadowLightmap.z
        ? smoothstep(ge_ShadowLightmap.z, ge_ShadowLightmap.w, depth)
        : step(ge_ShadowLightmap.w, depth);
    if (mode == 1) return mix(realtime, baked, fade);
    if (mode == 2) return baked * mix(realtime, 1.0, fade);
    return realtime;
}

vec3 GE_SurfaceDiffuseIrradiance(vec3 environment, vec3 ambientFloor)
{
    if (ge_BakedIrradiance.a <= 0.5)
        return environment + ambientFloor;
#ifdef GE_IBL_ENABLED
    return ge_BakedIrradiance.rgb;
#else
    return ge_BakedIrradiance.rgb * ge_BakedDiffuseWeight;
#endif
}
#endif
