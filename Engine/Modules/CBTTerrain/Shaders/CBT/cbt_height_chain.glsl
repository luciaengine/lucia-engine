// Chain levels use a clamped [1 2 1]/4 tent around sample 2j; lambda = log2(d*t/(f*s0)).
// Shared vertices compute identical heights; lambda <= 0.75 keeps cached height bits.
// The C++ shim supplies CBT_ChainLoad; CBT_CHAIN_LEVEL_RULE_ONLY omits chain taps.
#ifndef CBT_HEIGHT_CHAIN_GLSL
#define CBT_HEIGHT_CHAIN_GLSL

#ifndef CBT_CHAIN_LEVEL_RULE_ONLY
float CBT_ChainLoad(uint word);
#endif

// GE_SHARED_HEIGHT_CHAIN_BEGIN
// Mirror CBTLayout.h kCBTHeightChainHeaderWords and kCBTHeightChainMaxLevel.
const uint CBT_CHAIN_HEADER_WORDS = 4u;
const uint CBT_CHAIN_MAX_LEVEL = 15u;

// Samples along an axis of `level0Samples` at level `level` (mirror CBTHeightChainSamples).
uint CBT_ChainSamples(uint level0Samples, uint level)
{
    uint samples = level0Samples;
    for (uint l = 0u; l < level; ++l)
        samples = samples / 2u + 1u;
    return samples;
}

// The coarsest stored level (mirror CBTHeightChainTopLevel).
uint CBT_ChainTopLevel(uint samplesX, uint samplesZ)
{
    uint level = 0u;
    uint sx = samplesX;
    uint sz = samplesZ;
    while (level < CBT_CHAIN_MAX_LEVEL && (sx > 2u || sz > 2u))
    {
        sx = sx / 2u + 1u;
        sz = sz / 2u + 1u;
        level = level + 1u;
    }
    return level;
}

// The word of level `level`'s first sample, level >= 1 (mirror CBTHeightChainLevelWord).
uint CBT_ChainLevelWord(uint samplesX, uint samplesZ, uint level)
{
    uint word = CBT_CHAIN_HEADER_WORDS;
    uint sx = samplesX;
    uint sz = samplesZ;
    for (uint l = 1u; l < level; ++l)
    {
        sx = sx / 2u + 1u;
        sz = sz / 2u + 1u;
        word = word + sx * sz;
    }
    return word;
}

// The continuous level a vertex `distance` meters from the eye draws at: log2(distance x scale),
// scale = t / (f x s0) (mirror TerrainECS GeometryPageLevel). Negative is finer than level 0.
float CBT_ChainLevel(float distance, float scale)
{
    precise float edge = max(distance, 1.17549435e-38f) * scale;
    return log2(edge);
}

// The parent level's weight at continuous level `lambda`: 0 over the first three quarters of a
// level, a linear ramp to 1 over the last (mirror cbt_page.glsl CBT_PageParentBlend), so the surface
// reaches the next level exactly where the level ends and nothing pops as the eye moves.
float CBT_ChainParentBlend(float lambda)
{
    precise float inRing = lambda - floor(lambda);
    return clamp((inRing - 0.75f) / (1.0f - 0.75f), 0.0f, 1.0f);
}

// Normal-chain weight is zero through lambda 1.75 and one from lambda 2.
// The transition blends level 2 against the level-0 normal map; chain mip 0 is not sampled.
float CBT_ChainNormalWeight(float lambda)
{
    if (!(lambda > 1.75f))
        return 0.0f;
    if (lambda >= 2.0f)
        return 1.0f;
    return (lambda - 1.75f) / (2.0f - 1.75f);
}

// Grazing spread is 1/cos(view angle). Its shading footprint takes over across lambda 0..1.
// This distance band avoids a visible ring where the draw first reaches the filtered surface.
const float CBT_GRAZING_FULL_LEVEL = 1.0f;

// The share of the grazing footprint a fragment at continuous level `lambda` shades with: 0 at lambda
// 0 and nearer, 1 from CBT_GRAZING_FULL_LEVEL on, linear in the level between.
float CBT_GrazingShare(float lambda)
{
    return clamp(lambda / CBT_GRAZING_FULL_LEVEL, 0.0f, 1.0f);
}

// The level of the ground a fragment at `lambda` covers when it sees the ground with the grazing
// `spread` (>= 1): the level its normal is read at (CBT_ChainNormalWeight, cbt_surface.glsl).
float CBT_GrazingLevel(float lambda, float spread)
{
    return lambda + CBT_GrazingShare(lambda) * log2(spread);
}

#ifndef CBT_CHAIN_LEVEL_RULE_ONLY
// Level `level` (>= 1) at terrain (u, v), normalized: bilinear between the four lattice samples
// around the point. Level L's sample j sits on level-0 sample j x 2^L (cbt_page.glsl's lattice).
float CBT_ChainSampleLevel(float u, float v, uint level, uint samplesX, uint samplesZ)
{
    uint sx = CBT_ChainSamples(samplesX, level);
    uint sz = CBT_ChainSamples(samplesZ, level);
    precise float cx = clamp(u, 0.0f, 1.0f) * float(samplesX - 1u) / float(1u << level);
    precise float cz = clamp(v, 0.0f, 1.0f) * float(samplesZ - 1u) / float(1u << level);
    uint ix = min(uint(cx), sx - 2u);
    uint iz = min(uint(cz), sz - 2u);
    precise float fx = cx - float(ix);
    precise float fz = cz - float(iz);
    uint word = CBT_ChainLevelWord(samplesX, samplesZ, level) + iz * sx + ix;
    float h00 = CBT_ChainLoad(word);
    float h10 = CBT_ChainLoad(word + 1u);
    float h01 = CBT_ChainLoad(word + sx);
    float h11 = CBT_ChainLoad(word + sx + 1u);
    precise float h0 = h00 + (h10 - h00) * fx;
    precise float h1 = h01 + (h11 - h01) * fx;
    precise float h = h0 + (h1 - h0) * fz;
    return h;
}

// Vertices keep cached height through lambda 0.75, then blend in each level's last quarter.
// Chain heights are normalized: world height = sample*heightScale + originY.
float CBT_ChainDrawnHeight(float u, float v, float cachedHeight, float lambda, uint samplesX, uint samplesZ,
                           uint top, float heightScale, float originY)
{
    if (!(lambda > 0.75f) || top == 0u)
        return cachedHeight;
    float clamped = min(lambda, float(top));
    uint base = min(uint(clamped), top);
    float w = base < top ? CBT_ChainParentBlend(clamped) : 0.0f;
    precise float own = cachedHeight;
    if (base > 0u)
        own = CBT_ChainSampleLevel(u, v, base, samplesX, samplesZ) * heightScale + originY;
    if (!(w > 0.0f))
        return own;
    precise float parent = CBT_ChainSampleLevel(u, v, base + 1u, samplesX, samplesZ) * heightScale + originY;
    precise float drawn = own + (parent - own) * w;
    return drawn;
}

// Chain normals use a central difference across one level-L sample in each direction.
// cellX/cellZ are level-0 spacing in meters; normalized heights use heightScale.
vec2 CBT_ChainNormal(float u, float v, uint level, uint samplesX, uint samplesZ, float cellX, float cellZ,
                     float heightScale)
{
    float du = float(1u << level) / float(samplesX - 1u);
    float dv = float(1u << level) / float(samplesZ - 1u);
    float hL = CBT_ChainSampleLevel(u - du, v, level, samplesX, samplesZ);
    float hR = CBT_ChainSampleLevel(u + du, v, level, samplesX, samplesZ);
    float hD = CBT_ChainSampleLevel(u, v - dv, level, samplesX, samplesZ);
    float hU = CBT_ChainSampleLevel(u, v + dv, level, samplesX, samplesZ);
    float spanX = (min(u + du, 1.0f) - max(u - du, 0.0f)) * float(samplesX - 1u) * cellX;
    float spanZ = (min(v + dv, 1.0f) - max(v - dv, 0.0f)) * float(samplesZ - 1u) * cellZ;
    float dx = (hR - hL) * heightScale / max(spanX, 1.0e-6f);
    float dz = (hU - hD) * heightScale / max(spanZ, 1.0e-6f);
    float len = sqrt(dx * dx + 1.0f + dz * dz);
    return vec2(-dx / len, -dz / len);
}

#endif // CBT_CHAIN_LEVEL_RULE_ONLY
// GE_SHARED_HEIGHT_CHAIN_END

#endif // CBT_HEIGHT_CHAIN_GLSL
