#ifndef GE_VIRTUAL_SHADOW_PROJECTIONS_GLSL
#define GE_VIRTUAL_SHADOW_PROJECTIONS_GLSL

#include "virtual_shadow_lookup.glsl"
#include "screen_position.glsl"

struct GE_VirtualShadowProjection
{
    mat4 RelativeToLightClip;
    uvec4 Address;
    vec4 LightPositionRelAndFar;
};

// Mirrors VirtualShadowLookupHeaderGPU and VirtualShadowLookupProjectionGPU.
layout(set = 0, binding = 60, std430) readonly buffer VirtualShadowProjections
{
    uvec4 ge_virtualShadowDirectionalRows;
    uint ge_virtualShadowLocalRows[1024];
    GE_VirtualShadowProjection ge_virtualShadowProjections[];
};

bool GE_VirtualShadowHasProjection(uint row)
{
    return row != kGE_VirtualShadowMissingPage && row < uint(ge_virtualShadowProjections.length());
}

uint GE_VirtualShadowReceiverMip(GE_VirtualShadowProjection projection, vec4 clip,
                                 vec3 receiverDx, vec3 receiverDy)
{
    vec4 right = clip + projection.RelativeToLightClip * vec4(receiverDx, 0.0);
    vec4 down = clip + projection.RelativeToLightClip * vec4(receiverDy, 0.0);
    if (!(right.w > 0.0 && down.w > 0.0))
        return projection.Address.z;
    vec2 ndc = clip.xy / clip.w;
    vec2 dx = right.xy / right.w - ndc;
    vec2 dy = down.xy / down.w - ndc;
    float footprint = max(length(dx), length(dy)) * (0.5 * float(projection.Address.y * kGE_VirtualShadowPageTexels));
    return GE_VirtualShadowMip(footprint, projection.Address.z);
}

bool GE_VirtualShadowProject(mat4 projection, vec3 pos, float bias, out vec4 clip,
                             out vec2 uv, out float reference)
{
    clip = projection * vec4(pos, 1.0);
    uv = vec2(0.0);
    reference = 0.0;
    if (!(clip.w > 1e-6) || any(isnan(clip)) || any(isinf(clip)))
        return false;
    vec3 ndc = clip.xyz / clip.w;
    uv = GE_YUpNdcToViewportUV(ndc.xy);
    reference = ndc.z + bias;
    return GE_ViewportUVInside(uv) && reference >= 0.0 && reference <= 1.0;
}

#endif
