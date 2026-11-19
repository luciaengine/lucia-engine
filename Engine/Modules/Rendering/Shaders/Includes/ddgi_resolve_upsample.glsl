#ifndef GE_DDGI_RESOLVE_UPSAMPLE_GLSL
#define GE_DDGI_RESOLVE_UPSAMPLE_GLSL

// Upsamples the DDGI diffuse irradiance from resolve extent to one pixel. Shared by the forward consume (ibl.glsl)
// and its GPU regression fixture (ddgi_resolve_upsample_test.comp).
//
// The includer defines GE_DDGI_UPSAMPLE_IRRADIANCE: the blurred irradiance at resolve extent, rgb, with the raw
// reverse-Z depth in a. With GE_DDGI_UPSAMPLE_GATHER_NORMAL (the world normal each texel was gathered for, 0 where no
// surface is), a texel is kept only when the normal it was gathered for faces about as the pixel does. At a crease
// between a wall and a floor the depth has no step, so the depth test alone lets the light of one surface into the
// other. Without it, the depth test is the only guide.
//
// All reads are texel fetches: a filtered read mixes the 2 sides of a crease before the test can reject one.

// Reverse-Z is non-linear, so the depth tolerance is relative to the pixel depth (as in the blur).
const float kDDGIUpsampleDepthTolerance = 0.02;
// A texel is kept when the cosine to the pixel normal is at most this much below the best of the 4 texels. A
// normal-mapped gather normal keeps its texels; a texel of the other side of a crease does not.
const float kDDGIUpsampleNormalMargin = 0.1;
// A texel at 0 bilinear weight still counts when it is the only one kept.
const float kDDGIUpsampleMinWeight = 1.0e-3;
// The agreement of a texel that describes another surface.
const float kDDGIUpsampleRejected = -2.0;

// The irradiance for the pixel at `uv` (0..1 over the render extent) with raw depth `rawDepth` and geometric world
// normal `pixelN`. `pixelN` is read only with GE_DDGI_UPSAMPLE_GATHER_NORMAL.
vec3 GE_DDGIUpsampleIrradiance(vec2 uv, float rawDepth, vec3 pixelN)
{
    // The 4 resolve texels around the pixel centre, for any ratio of the 2 extents.
    ivec2 inSize = textureSize(GE_DDGI_UPSAMPLE_IRRADIANCE, 0);
    vec2 footprint = uv * vec2(inSize) - 0.5;
    ivec2 base = ivec2(floor(footprint));
    vec2 frac = footprint - vec2(base);

    vec3 taps[4];
    float agreement[4];
    float best = kDDGIUpsampleRejected;
    for (int k = 0; k < 4; ++k)
    {
        ivec2 texel = clamp(base + ivec2(k & 1, k >> 1), ivec2(0), inSize - ivec2(1));
        vec4 tap = texelFetch(GE_DDGI_UPSAMPLE_IRRADIANCE, texel, 0);
        taps[k] = tap.rgb;
        bool sameDepth = tap.a > 0.0 && abs(tap.a - rawDepth) <= kDDGIUpsampleDepthTolerance * max(rawDepth, 1.0e-6);
#if defined(GE_DDGI_UPSAMPLE_GATHER_NORMAL)
        vec3 gatherN = texelFetch(GE_DDGI_UPSAMPLE_GATHER_NORMAL, texel, 0).xyz;
        agreement[k] = sameDepth && dot(gatherN, gatherN) > 0.25 ? dot(pixelN, normalize(gatherN))
                                                                  : kDDGIUpsampleRejected;
#else
        agreement[k] = sameDepth ? 1.0 : kDDGIUpsampleRejected;
#endif
        best = max(best, agreement[k]);
    }

    if (best <= kDDGIUpsampleRejected)
    {
        // No texel describes this surface (a sliver thinner than a resolve texel): take the nearest texel, which is
        // at most one resolve texel away.
        ivec2 nearest = clamp(base + ivec2(round(frac)), ivec2(0), inSize - ivec2(1));
        return texelFetch(GE_DDGI_UPSAMPLE_IRRADIANCE, nearest, 0).rgb;
    }
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        if (agreement[k] < best - kDDGIUpsampleNormalMargin)
            continue;
        float wx = (k & 1) == 0 ? 1.0 - frac.x : frac.x;
        float wy = (k >> 1) == 0 ? 1.0 - frac.y : frac.y;
        float w = max(wx * wy, kDDGIUpsampleMinWeight);
        sum += taps[k] * w;
        wsum += w;
    }
    return sum / wsum;
}

#endif
