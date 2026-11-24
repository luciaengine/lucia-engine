#ifndef GE_FOLIAGE_COVERAGE_GLSL
#define GE_FOLIAGE_COVERAGE_GLSL

#include "integer_hash.glsl"

// A coverage threshold in [0, 1) fixed to the screen pixel. It does not move with the surface, so
// use it only for a surface that faces the camera, such as an impostor card.
float GE_FoliageScreenThreshold(vec2 fragCoord)
{
    return fract(52.9829189 * fract(dot(floor(fragCoord), vec2(0.06711056, 0.00583715))));
}

// The leaf sample keeps at least 32 texels across. Coarser levels spread opacity
// into the texture border and flatten the outline of the canopy.
const float kFoliageShapeFootprint = 1.0 / 32.0;

// The isotropic UV gradient of a sample at the shape bound, under mip bias `mipBias`.
float GE_FoliageShapeGradient(float mipBias)
{
    return kFoliageShapeFootprint * exp2(-mipBias);
}

// The anisotropy of the material samplers (RendererProfile's material anisotropy).
const float kFoliageMaxAnisotropy = 16.0;

// UV per pixel of the level a material sampler reads for screen derivatives `uvDx` and `uvDy` and
// mip bias `mipBias`: the shorter axis, or the longer axis over the anisotropy where that is coarser.
// A card seen edge-on near the camera keeps its fine level and its cutoff test.
float GE_FoliageFootprint(vec2 uvDx, vec2 uvDy, float mipBias)
{
    float lengthX = length(uvDx);
    float lengthY = length(uvDy);
    float level = max(min(lengthX, lengthY), max(lengthX, lengthY) / kFoliageMaxAnisotropy);
    return max(level, 1e-6) * exp2(mipBias);
}

// The coverage test blends in from 32 to 16 texels across the footprint.
float GE_FoliageTailWeight(float footprint)
{
    return 1.0 - smoothstep(16.0, 32.0, 1.0 / footprint);
}

// Blend the cutoff mask with filtered alpha. With a uniform threshold per leaf,
// the expected coverage of the filtered alpha equals its mean.
float GE_FoliageCoverageOpacity(float alpha, float cutoff, float coarseWeight, float threshold)
{
    float cutout = step(cutoff, alpha);
    float coverage = mix(cutout, alpha, coarseWeight);
    return coverage > threshold ? 1.0 : 0.0;
}

// Reject the weak alpha fringe of a filtered leaf shape before the stochastic coverage test.
float GE_FoliageLeafCoverageOpacity(float shapeAlpha, float alphaScale, float cutoff, float coarseWeight, float threshold)
{
    const float kMinimumAlphaFractionOfCutoff = 0.75;
    float shapeMask = step(kMinimumAlphaFractionOfCutoff * cutoff, shapeAlpha);
    return shapeMask * GE_FoliageCoverageOpacity(shapeAlpha * alphaScale, cutoff, coarseWeight, threshold);
}

// The hash of the provoking vertex and its GPUScene slot supplies a stable triangle seed.
uint GE_MaskCoverageSeed(uint vertexIndex, uint instanceIndex)
{
    return GE_HashU32(vertexIndex ^ GE_HashU32(instanceIndex));
}

// The far opacity of a masked surface: texture alpha `alpha` times `alphaScale` (tint and vertex alpha)
// against `cutoff`. The scale only moves the cutoff, as near, so a scale under the cutoff hides the surface.
float GE_MaskCoverageOpacity(float alpha, float alphaScale, float cutoff, float coarseWeight, float threshold)
{
    return step(cutoff, alphaScale) * GE_FoliageCoverageOpacity(alpha, cutoff / max(alphaScale, 1e-6), coarseWeight, threshold);
}

// The coverage threshold in [0, 1) of a seed word: its top 24 bits, which a float holds exactly.
float GE_MaskCoverageThreshold(uint seed)
{
    return float(seed >> 8u) / 16777216.0;
}

#endif
