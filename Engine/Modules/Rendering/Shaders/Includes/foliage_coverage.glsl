#ifndef GE_FOLIAGE_COVERAGE_GLSL
#define GE_FOLIAGE_COVERAGE_GLSL

// A coverage threshold in [0, 1) fixed to the screen pixel. It does not move with the surface, so
// use it only for a surface that faces the camera, such as an impostor card.
float GE_FoliageScreenThreshold(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(floor(fragCoord), vec2(0.06711056, 0.00583715))));
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
// stays at the base coverage, also where the filtered alpha is below the cutoff. The threshold can
// be one value per pixel or one value per leaf: with a uniform value per leaf, the expected covered
// area of each leaf is its base coverage.
float GE_FoliageCoverageOpacity(float alpha, float cutoff, float coarseWeight, float threshold)
{
    float cutout = step(cutoff, alpha);
    float coverage = mix(cutout, alpha, coarseWeight);
    return coverage > threshold ? 1.0 : 0.0;
}

#endif
