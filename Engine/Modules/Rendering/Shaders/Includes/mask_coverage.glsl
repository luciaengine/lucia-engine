#ifndef GE_MASK_COVERAGE_GLSL
#define GE_MASK_COVERAGE_GLSL

// A coverage threshold in [0, 1) fixed to the screen pixel. It does not move with the surface, so
// use it only for a surface that faces the camera, such as an impostor card.
float GE_MaskCoverageScreenThreshold(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(floor(fragCoord), vec2(0.06711056, 0.00583715))));
}

// The leaf sample keeps at least 32 texels across. Coarser levels spread opacity
// into the texture border and flatten the outline of the canopy.
const float kFoliageShapeFootprint = 1.0 / 32.0;

// UV per pixel of a sample with screen derivatives `uvDx` and `uvDy` and mip bias `mipBias`, along
// the longer axis. The sampler reads the level with about 1 / footprint texels across.
float GE_MaskCoverageMajorFootprint(vec2 uvDx, vec2 uvDy, float mipBias)
{
    return max(max(length(uvDx), length(uvDy)), 1e-6) * exp2(mipBias);
}

// The coverage test blends in from 32 to 16 texels across the footprint.
float GE_MaskCoverageTailWeight(float footprint)
{
    return 1.0 - smoothstep(16.0, 32.0, 1.0 / footprint);
}

// Blend the cutoff mask with filtered alpha. With a uniform threshold per leaf,
// the expected coverage of the filtered alpha equals its mean.
float GE_MaskCoverageTestOpacity(float alpha, float cutoff, float coarseWeight, float threshold)
{
    float cutout = step(cutoff, alpha);
    float coverage = mix(cutout, alpha, coarseWeight);
    return coverage > threshold ? 1.0 : 0.0;
}

// Reject the weak alpha fringe of a filtered leaf shape before the stochastic coverage test.
float GE_MaskCoverageLeafOpacity(float shapeAlpha, float alphaScale, float cutoff, float coarseWeight, float threshold)
{
    const float kMinimumAlphaFractionOfCutoff = 0.75;
    float shapeMask = step(kMinimumAlphaFractionOfCutoff * cutoff, shapeAlpha);
    return shapeMask * GE_MaskCoverageTestOpacity(shapeAlpha * alphaScale, cutoff, coarseWeight, threshold);
}

#endif
