// The achromatic response of each look in tonemap.frag at the SDR tier, and its inverse. x is exposed
// scene luminance (stored luma x the exposure / the pre-exposure); y is the display-linear light of a grey.
// A new look adds its response and inverse pair here.
#ifndef GE_DISPLAY_RESPONSE_GLSL
#define GE_DISPLAY_RESPONSE_GLSL

#include "tonemap_aces2_tables.glsl"
#include "tonemap_look.glsl"

// The largest y an inverse accepts, so that each inverse returns a finite x. Neutral reaches 1.0 only at
// an infinite x; the tonescale curves reach 1.0 at a finite x.
const float kDisplayResponseMaxY = 0.9999999;

// Khronos PBR Neutral on a grey: a quadratic toe below 0.08, linear to 0.76, a hyperbolic shoulder. Exact.
float GE_NeutralResponse(float x)
{
    float v = x < 0.08 ? 6.25 * x * x : x - 0.04;
    return v < 0.76 ? v : 1.0 - 0.0576 / (v - 0.52);
}

float GE_NeutralResponseInverse(float y)
{
    if (y < 0.04)
        return 0.4 * sqrt(y);
    if (y < 0.76)
        return y + 0.04;
    return 0.0576 / (1.0 - y) + 0.56;
}

// The ACES 2 tonescale on a grey: f = m (x / (x + s))^g, y = min(f^2 / (f + t), 1). k = (m, s, g, t).
float GE_TonescaleResponse(float x, vec4 k)
{
    float f = k.x * pow(x / (x + k.y), k.z);
    return min(f * f / (f + k.w), 1.0);
}

// Exact for y <= 1: f stays below m, so x is finite.
float GE_TonescaleResponseInverse(float y, vec4 k)
{
    float f = 0.5 * (y + sqrt(y * y + 4.0 * y * k.w));
    float u = pow(f / k.x, 1.0 / k.z);
    return k.y * u / (1.0 - u);
}

// On a grey, the tonescale input in tonemap_aces2.glsl is x * 2^(-1/3), so the half point on x is tsSDiv * 2^(1/3).
const float kAces2HalfPointScale = 1.25992105;

// Filmic: the ACES 2 tonescale at the 100-nit SDR tier. m and s come from tier 0 of the Aces2Tables buffer
// (set 0, binding 2); g and t are the fixed values of tonemap_aces2.glsl. Exact on a grey.
vec4 Aces2SdrTonescale()
{
    return vec4(aces2_tier_scalars[0][0], aces2_tier_scalars[0][1] * kAces2HalfPointScale, 1.15, 0.04);
}

// Photographic: AgX's polynomial sigmoid has no closed inverse. The tonescale form fit to it on a grey
// stays within 2.8 sRGB-encoded 8-bit levels of AgX over x in [1e-3, 1e3].
const vec4 kAgxFitTonescale = vec4(1.05358, 0.70724, 0.91618, 0.04284);

// The look's display light for exposed scene luminance x >= 0. Neutral is the default look; the
// no-look arm is the identity.
float GE_DisplayResponse(int look, float x)
{
    switch (look)
    {
        case kLookFilmic: return GE_TonescaleResponse(x, Aces2SdrTonescale());
        case kLookPhotographic: return GE_TonescaleResponse(x, kAgxFitTonescale);
        case kNoLookArm: return x;
        default: return GE_NeutralResponse(x);
    }
}

// The exposed scene luminance the look shows as display light y >= 0.
float GE_DisplayResponseInverse(int look, float y)
{
    if (look == kNoLookArm)
        return y;
    y = min(y, kDisplayResponseMaxY);
    switch (look)
    {
        case kLookFilmic: return GE_TonescaleResponseInverse(y, Aces2SdrTonescale());
        case kLookPhotographic: return GE_TonescaleResponseInverse(y, kAgxFitTonescale);
        default: return GE_NeutralResponseInverse(y);
    }
}

#endif // GE_DISPLAY_RESPONSE_GLSL
