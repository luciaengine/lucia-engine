#ifndef GE_FOLIAGE_COVERAGE_GLSL
#define GE_FOLIAGE_COVERAGE_GLSL

float GE_FoliageScreenThreshold(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(floor(fragCoord), vec2(0.06711056, 0.00583715))));
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

// Weight of the coverage test for a leaf texture of `texels` across, read at mip level `mip`: 0
// while the level keeps 16 texels or more across, 1 at 4 texels or fewer. The cutoff test keeps the
// coverage of a 0/1 alpha mask down to about 16 texels; below that the filtered alpha falls under
// the cutoff.
float GE_FoliageTailWeight(float mip, float texels)
{
    return 1.0 - smoothstep(4.0, 16.0, texels * exp2(-mip));
}

// Opacity, 0 or 1, of an alpha-tested foliage sample. `coarseWeight` 0 is the material cutoff
// test; 1 is a screen-space threshold on the filtered alpha. A filtered texel of a 0/1 alpha mask
// holds the share of covered texels under it, so the share of passing pixels stays at the base
// coverage, also where the filtered alpha is below the cutoff. The threshold depends only on the
// pixel and on `layerSeed`, so every pass keeps the same pixels. A seed in [0, 1) turns the
// threshold, so overlapping surfaces with different seeds do not pass together; over random
// seeds their coverage adds up as the coverage of separate leaves does.
float GE_FoliageCoverageOpacity(float alpha, float cutoff, float coarseWeight, vec2 fragCoord, float layerSeed)
{
    float cutout = step(cutoff, alpha);
    float coverage = mix(cutout, alpha, coarseWeight);
    return coverage > fract(GE_FoliageScreenThreshold(fragCoord) + layerSeed) ? 1.0 : 0.0;
}

#endif
