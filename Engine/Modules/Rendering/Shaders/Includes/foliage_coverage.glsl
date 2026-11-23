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

// How far a sample at mip level `mip` is from a cutoff test to a coverage test: 0 near the base
// level, 1 from level 2.
float GE_FoliageCoarseWeight(float mip)
{
    return smoothstep(0.5, 2.0, mip);
}

// Opacity, 0 or 1, of an alpha-tested foliage sample at mip level `mip`. Near the base level the
// test is the material cutoff. At coarse levels it is a screen-space threshold on the filtered
// alpha. A filtered texel of a 0/1 alpha mask holds the share of covered texels under it, so the
// share of passing pixels stays at the base coverage at every level, also where the filtered
// alpha is below the cutoff. The threshold depends only on the pixel and on `layerSeed`, so every
// pass keeps the same pixels. Overlapping surfaces with different seeds in [0, 1) pass
// independently, so their coverage adds up as the coverage of separate leaves does.
float GE_FoliageCoverageOpacity(float alpha, float cutoff, float mip, vec2 fragCoord, float layerSeed)
{
    float cutout = step(cutoff, alpha);
    float coverage = mix(cutout, alpha, GE_FoliageCoarseWeight(mip));
    return coverage > fract(GE_FoliageScreenThreshold(fragCoord) + layerSeed) ? 1.0 : 0.0;
}

#endif
