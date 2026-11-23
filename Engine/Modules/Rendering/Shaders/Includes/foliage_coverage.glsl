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

// UV per pixel of a sample with screen derivatives `uvDx` and `uvDy` and mip bias `mipBias`, along
// the longer axis. The sampler reads the level with about 1 / footprint texels across.
float GE_FoliageFootprint(vec2 uvDx, vec2 uvDy, float mipBias)
{
    return max(max(length(uvDx), length(uvDy)), 1e-6) * exp2(mipBias);
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

// The coverage seed of a masked surface without a leaf seed: one uniform threshold per triangle for
// GE_FoliageCoverageOpacity. The vertex stage hashes the vertex index with the GPUScene instance index,
// and the flat varying takes the provoking vertex's value. So the seed moves with the surface, is the
// same in the depth, shadow and color passes, and differs between overlapping triangles and
// instances. The two triangles of a quad in fan order start at one vertex and share one seed. The
// compat profile has no screen derivatives (GE_DFDX reads zero), so it keeps the cutoff test.
#if defined(ALPHA_TEST) && !defined(GE_COMPAT_PROFILE)
#define GE_MASK_COVERAGE_SEED
#endif

// The seed word of vertex `vertexIndex` of GPUScene instance `instanceIndex`.
uint GE_MaskCoverageSeed(uint vertexIndex, uint instanceIndex)
{
    return GE_HashU32(vertexIndex ^ GE_HashU32(instanceIndex));
}

// The coverage threshold in [0, 1) of a seed word: its top 24 bits, which a float holds exactly.
float GE_MaskCoverageThreshold(uint seed)
{
    return float(seed >> 8u) / 16777216.0;
}

#endif
