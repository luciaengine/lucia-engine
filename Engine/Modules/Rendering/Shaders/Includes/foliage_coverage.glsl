#ifndef GE_FOLIAGE_COVERAGE_GLSL
#define GE_FOLIAGE_COVERAGE_GLSL

#include "integer_hash.glsl"

// A coverage threshold in [0, 1) fixed to the screen pixel. It does not move with the surface, so
// use it only for a surface that faces the camera, such as an impostor card.
float GE_FoliageScreenThreshold(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(floor(fragCoord), vec2(0.06711056, 0.00583715))));
}

// A coverage threshold in [0, 1) fixed to the surface (hashed alpha testing, Wyman and McGuire
// 2017). The hash cells are UV squares of about one pixel, at the two power-of-2 scales around the
// pixel footprint; the two values are blended by the fraction between the scales and mapped back to
// a uniform distribution. The pattern moves with the surface, so it does not crawl when the camera
// moves, and it changes smoothly with distance. Different `layerSeed` values give unrelated
// patterns, so overlapping surfaces pass independently. The same UV, derivatives and seed give the
// same value in every pass.
float GE_FoliageSurfaceThreshold(vec2 uv, vec2 uvDx, vec2 uvDy, float layerSeed)
{
    float pixelScale = log2(1.0 / max(max(length(uvDx), length(uvDy)), 1e-6));
    float lowScale = floor(pixelScale);
    uint seedKey = GE_HashU32(uint(layerSeed * 16777216.0));
    vec2 lowCell = floor(uv * exp2(lowScale));
    vec2 highCell = floor(uv * exp2(lowScale + 1.0));
    uint lowKey = GE_HashU32(seedKey ^ GE_HashU32(uint(int(lowCell.x)) ^ GE_HashU32(uint(int(lowCell.y)) + uint(int(lowScale)))));
    uint highKey = GE_HashU32(seedKey ^ GE_HashU32(uint(int(highCell.x)) ^ GE_HashU32(uint(int(highCell.y)) + uint(int(lowScale) + 1))));
    float blend = pixelScale - lowScale;
    float x = mix(float(lowKey >> 8u) / 16777216.0, float(highKey >> 8u) / 16777216.0, blend);
    // The sum of two uniform values is not uniform; its distribution function maps it back.
    float a = min(blend, 1.0 - blend);
    float threshold = x;
    if (a > 1e-4)
    {
        if (x < a)
            threshold = x * x / (2.0 * a * (1.0 - a));
        else if (x < 1.0 - a)
            threshold = (x - 0.5 * a) / (1.0 - a);
        else
            threshold = 1.0 - (1.0 - x) * (1.0 - x) / (2.0 * a * (1.0 - a));
    }
    return clamp(threshold, 0.0, 0.999999);
}

// The mip level that a sample reads in a texture of `texels`, from the UV derivatives of the
// sample along the screen axes. The larger axis sets the level, as for an isotropic sampler.
float GE_FoliageMipLevel(vec2 uvDx, vec2 uvDy, vec2 texels)
{
    vec2 dx = uvDx * texels;
    vec2 dy = uvDy * texels;
    return 0.5 * log2(max(1.0, max(dot(dx, dx), dot(dy, dy))));
}

// Weight of the coverage test for an impostor atlas tile: 0 up to mip level 0.5, 1 from level 2.
float GE_FoliageCoarseWeight(float mip)
{
    return smoothstep(0.5, 2.0, mip);
}

// Weight of the coverage test for a leaf texture sampled at UV with screen derivatives `uvDx` and
// `uvDy` and mip bias `mipBias`: 0 while the read level keeps 16 texels or more across, 1 at 4
// texels or fewer. The cutoff test keeps the coverage of a 0/1 alpha mask down to about 16 texels;
// below that the filtered alpha falls under the cutoff. The sampler reads the level where one texel
// covers one pixel, so that level has about 1 / (UV per pixel) texels across, whatever the size of
// the texture. In an atlas of several leaf tiles the change comes later than the tile needs.
float GE_FoliageTailWeight(vec2 uvDx, vec2 uvDy, float mipBias)
{
    float uvPerPixel = max(max(length(uvDx), length(uvDy)), 1e-6);
    return 1.0 - smoothstep(4.0, 16.0, exp2(-mipBias) / uvPerPixel);
}

// Opacity, 0 or 1, of an alpha-tested foliage sample. `coarseWeight` 0 is the material cutoff
// test; 1 compares the filtered alpha with `threshold`, a uniform value in [0, 1). A filtered texel
// of a 0/1 alpha mask holds the share of covered texels under it, so the share of passing pixels
// stays at the base coverage, also where the filtered alpha is below the cutoff.
float GE_FoliageCoverageOpacity(float alpha, float cutoff, float coarseWeight, float threshold)
{
    float cutout = step(cutoff, alpha);
    float coverage = mix(cutout, alpha, coarseWeight);
    return coverage > threshold ? 1.0 : 0.0;
}

#endif
