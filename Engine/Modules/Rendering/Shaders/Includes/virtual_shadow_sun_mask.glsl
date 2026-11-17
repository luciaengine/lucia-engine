#ifndef GE_VIRTUAL_SHADOW_SUN_MASK_GLSL
#define GE_VIRTUAL_SHADOW_SUN_MASK_GLSL

#include "prepass_receiver_mask.glsl"

// The value byte of a sun mask texel (prepass_receiver_mask.glsl). 0 to 254: the virtual page
// visibility in steps of 1/254. 255: no resident page covers the receiver; the cascade applies.
const uint kGE_VirtualShadowSunMaskNoPage = 255u;
const float kGE_VirtualShadowSunMaskSteps = 254.0;

uint GE_EncodeVirtualShadowSunMask(bool resolved, float visibility)
{
    return resolved ? uint(round(clamp(visibility, 0.0, 1.0) * kGE_VirtualShadowSunMaskSteps))
                    : kGE_VirtualShadowSunMaskNoPage;
}

#ifdef GE_VIRTUAL_SHADOW_SUN_MASK
// Written by virtual_shadow_sun_mask.comp from the prepass depth, at the prepass resolution.
layout(set = 0, binding = 66) uniform usampler2D ge_virtualShadowSunMask;

// True with the visibility of a matched texel; false when no resident page covers its receiver.
bool GE_VirtualShadowSunMaskVisibility(uint packed, out float visibility)
{
    uint value = GE_PrepassReceiverValue(packed);
    visibility = value == kGE_VirtualShadowSunMaskNoPage ? 1.0 : float(value) / kGE_VirtualShadowSunMaskSteps;
    return value != kGE_VirtualShadowSunMaskNoPage;
}

// True with the virtual page visibility of the fragment's receiver texel; false when the cascade applies.
// The texel is the fragment's own pixel. If the fragment is not its receiver, the texel is the
// neighbour whose receiver is nearest the fragment's depth plane: the resolved MSAA depth is sample 0,
// so on an edge pixel a fragment can own only the other samples.
// depthGradient = (dFdx, dFdy) of gl_FragCoord.z.
bool GE_ReadVirtualShadowSunMask(vec2 screenPos, float fragmentDepth, vec2 depthGradient, out float visibility)
{
    visibility = 1.0;
    ivec2 pixel = ivec2(screenPos);
    ivec2 size = textureSize(ge_virtualShadowSunMask, 0);
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size)))
        return false;
    float footprint = GE_PrepassReceiverFootprint(depthGradient);
    uint packed = texelFetch(ge_virtualShadowSunMask, pixel, 0).r;
    if (GE_IsPrepassReceiver(packed, fragmentDepth, footprint))
        return GE_VirtualShadowSunMaskVisibility(packed, visibility);
    float bestError = -1.0;
    uint best = 0u;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 neighbour = pixel + ivec2(x, y);
            if ((x == 0 && y == 0) || any(lessThan(neighbour, ivec2(0))) || any(greaterThanEqual(neighbour, size)))
                continue;
            uint texel = texelFetch(ge_virtualShadowSunMask, neighbour, 0).r;
            float planeDepth = fragmentDepth + dot(depthGradient, vec2(x, y));
            if (!GE_IsPrepassReceiver(texel, planeDepth, footprint))
                continue;
            float error = abs(uintBitsToFloat(texel & 0xffffff00u) - planeDepth);
            if (bestError < 0.0 || error < bestError)
            {
                bestError = error;
                best = texel;
            }
        }
    }
    return bestError >= 0.0 && GE_VirtualShadowSunMaskVisibility(best, visibility);
}
#endif

#endif
