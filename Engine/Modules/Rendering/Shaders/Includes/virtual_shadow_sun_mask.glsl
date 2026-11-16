#ifndef GE_VIRTUAL_SHADOW_SUN_MASK_GLSL
#define GE_VIRTUAL_SHADOW_SUN_MASK_GLSL

#include "prepass_receiver_mask.glsl"

// The value byte of a virtual shadow sun mask texel (prepass_receiver_mask.glsl).
// 0 to 254: the resident virtual page visibility in steps of 1/254.
// 255: no resident page covers the receiver, so the cascade shadow applies.
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

// The sun visibility that the mask pass resolved for this pixel.
// Returns 1 with the visibility, 0 when no resident page covers the receiver
// (the cascade applies), and -1 when the fragment is not the prepass receiver
// of its pixel (the caller runs GE_TryVirtualShadowDirectional).
int GE_ReadVirtualShadowSunMask(vec2 screenPos, float fragmentDepth, float footprint, out float visibility)
{
    visibility = 1.0;
    ivec2 pixel = ivec2(screenPos);
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, textureSize(ge_virtualShadowSunMask, 0))))
        return -1;
    uint packed = texelFetch(ge_virtualShadowSunMask, pixel, 0).r;
    if (!GE_IsPrepassReceiver(packed, fragmentDepth, footprint))
        return -1;
    uint value = GE_PrepassReceiverValue(packed);
    if (value == kGE_VirtualShadowSunMaskNoPage)
        return 0;
    visibility = float(value) / kGE_VirtualShadowSunMaskSteps;
    return 1;
}
#endif

#endif
