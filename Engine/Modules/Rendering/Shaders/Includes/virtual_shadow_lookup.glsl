#ifndef GE_VIRTUAL_SHADOW_LOOKUP_GLSL
#define GE_VIRTUAL_SHADOW_LOOKUP_GLSL

// Default to the existing shadow comparison sampler. The raw override is for
// GPU comparison instruments; cross-page samples still resolve texels explicitly.
#if !defined(GE_VIRTUAL_SHADOW_RAW_COMPARE) && !defined(GE_VIRTUAL_SHADOW_HARDWARE_COMPARE)
#define GE_VIRTUAL_SHADOW_HARDWARE_COMPARE
#endif

#ifndef GE_VIRTUAL_SHADOW_DEPTH_BINDING
#define GE_VIRTUAL_SHADOW_DEPTH_BINDING 58
#endif
#ifndef GE_VIRTUAL_SHADOW_TABLE_BINDING
#define GE_VIRTUAL_SHADOW_TABLE_BINDING 59
#endif

// A layer contains one complete page. The table is in virtual-address order,
// with all fine pages followed by each successive coarse mip. A missing page
// contains 0xffffffff and must never be read from the depth array.
layout(set = 0, binding = GE_VIRTUAL_SHADOW_DEPTH_BINDING) uniform sampler2DArray ge_virtualShadowDepth;
layout(set = 0, binding = GE_VIRTUAL_SHADOW_TABLE_BINDING, std430) readonly buffer VirtualShadowPageTable
{
    uint ge_virtualShadowTiles[];
};

#ifdef GE_VIRTUAL_SHADOW_HARDWARE_COMPARE
layout(set = 0, binding = 61) uniform sampler2DArrayShadow ge_virtualShadowDepthCompare;
#endif

const uint kGE_VirtualShadowPageTexels = 128u;
const uint kGE_VirtualShadowMissingPage = 0xffffffffu;

uint GE_VirtualShadowMipStart(uint firstPage, uint side, uint mip)
{
    for (uint level = 0u; level < mip; ++level)
    {
        uint mipSide = side >> level;
        firstPage += mipSide * mipSide;
    }
    return firstPage;
}

// Select a mip from an already projected one-pixel footprint. Its units are
// the finest virtual mip's texels, as in the receiver request pass.
uint GE_VirtualShadowMip(float footprint, uint lastMip)
{
    if (isnan(footprint) || isinf(footprint))
        return lastMip;
    return uint(clamp(floor(log2(max(footprint, 1.0))), 0.0, float(lastMip)));
}

uint GE_VirtualShadowResolveTile(uint firstPage, uint side, uvec2 page, uint layerCount)
{
    uint address = firstPage + page.y * side + page.x;
    if (address < firstPage || address >= uint(ge_virtualShadowTiles.length()))
        return kGE_VirtualShadowMissingPage;
    uint tile = ge_virtualShadowTiles[address];
    return tile < layerCount ? tile : kGE_VirtualShadowMissingPage;
}

bool GE_VirtualShadowCompareTexel(uint firstPage, uint side, ivec2 texel, float reference,
                                 uint layerCount, out float visible)
{
    uvec2 page = uvec2(texel) / kGE_VirtualShadowPageTexels;
    uint tile = GE_VirtualShadowResolveTile(firstPage, side, page, layerCount);
    visible = 0.0;
    if (tile == kGE_VirtualShadowMissingPage)
        return false;

    ivec2 withinPage = texel % int(kGE_VirtualShadowPageTexels);
    float storedDepth = texelFetch(ge_virtualShadowDepth, ivec3(withinPage, int(tile)), 0).r;
    visible = reference >= storedDepth ? 1.0 : 0.0;
    return true;
}

// A hardware-PCF kernel may reuse one page only when every bilinear
// contribution fits within that resident page, including its half-texel border.
struct GE_VirtualShadowKernelPage
{
    uint Layer;
    float Resolution;
    vec2 TexelOrigin;
};

bool GE_VirtualShadowResolveKernelPage(uvec3 projection, uint mip, vec2 centerUV, float radiusUV,
                                       float reference, out GE_VirtualShadowKernelPage page)
{
    uint baseSide = projection.y;
    if (baseSide == 0u || baseSide > 128u || (baseSide & (baseSide - 1u)) != 0u ||
        mip > projection.z || mip > uint(findMSB(baseSide)) ||
        any(isnan(centerUV)) || any(isinf(centerUV)) ||
        isnan(radiusUV) || isinf(radiusUV) || radiusUV < 0.0 || radiusUV > 1.0 ||
        isnan(reference) || isinf(reference))
        return false;
    uint firstPage = GE_VirtualShadowMipStart(projection.x, baseSide, mip);
    if (firstPage < projection.x)
        return false;
    uint side = baseSide >> mip;
    int resolution = int(side * kGE_VirtualShadowPageTexels);
    ivec2 lo = ivec2(floor((centerUV - radiusUV) * float(resolution) - 0.5));
    ivec2 hi = ivec2(floor((centerUV + radiusUV) * float(resolution) - 0.5)) + 1;
    if (any(lessThan(lo, ivec2(0))) || any(greaterThanEqual(hi, ivec2(resolution))))
        return false;
    uvec2 lowPage = uvec2(lo) / kGE_VirtualShadowPageTexels;
    if (any(notEqual(lowPage, uvec2(hi) / kGE_VirtualShadowPageTexels)))
        return false;
    uint layer = GE_VirtualShadowResolveTile(firstPage, side, lowPage,
                                            uint(textureSize(ge_virtualShadowDepth, 0).z));
    if (layer == kGE_VirtualShadowMissingPage)
        return false;
    page.Layer = layer;
    page.Resolution = float(resolution);
    page.TexelOrigin = vec2(lowPage * kGE_VirtualShadowPageTexels);
    return true;
}

float GE_VirtualShadowCompareKernelPage(GE_VirtualShadowKernelPage page, vec2 uv, float reference)
{
#ifdef GE_VIRTUAL_SHADOW_HARDWARE_COMPARE
    vec2 localUV = (uv * page.Resolution - page.TexelOrigin) / float(kGE_VirtualShadowPageTexels);
    return textureGrad(ge_virtualShadowDepthCompare, vec4(localUV, float(page.Layer), reference), vec2(0.0), vec2(0.0));
#else
    vec2 texel = uv * page.Resolution - 0.5;
    vec2 weight = fract(texel);
    vec2 center = (floor(texel) - page.TexelOrigin + 1.0) / float(kGE_VirtualShadowPageTexels);
    vec4 depths = textureGather(ge_virtualShadowDepth, vec3(center, float(page.Layer)));
    vec4 compare = vec4(greaterThanEqual(vec4(reference), depths));
    return mix(mix(compare.w, compare.z, weight.x), mix(compare.x, compare.y, weight.x), weight.y);
#endif
}

// A kernel that crosses a page edge reads at most the 2 x 2 pages from its lowest page. Each
// page is resolved once; a tap then reads its own page, or four texels across an edge.
struct GE_VirtualShadowKernelQuad
{
    uint Layers[4];
    float Resolution;
    vec2 TexelOrigin;
};

bool GE_VirtualShadowResolveKernelQuad(uvec3 projection, uint mip, vec2 centerUV, float radiusUV,
                                       out GE_VirtualShadowKernelQuad quad)
{
    uint baseSide = projection.y;
    if (baseSide == 0u || baseSide > 128u || (baseSide & (baseSide - 1u)) != 0u ||
        mip > projection.z || mip > uint(findMSB(baseSide)) ||
        any(isnan(centerUV)) || any(isinf(centerUV)) ||
        isnan(radiusUV) || isinf(radiusUV) || radiusUV < 0.0 || radiusUV > 1.0)
        return false;
    uint firstPage = GE_VirtualShadowMipStart(projection.x, baseSide, mip);
    if (firstPage < projection.x)
        return false;
    uint side = baseSide >> mip;
    int resolution = int(side * kGE_VirtualShadowPageTexels);
    ivec2 lo = ivec2(floor((centerUV - radiusUV) * float(resolution) - 0.5));
    ivec2 hi = ivec2(floor((centerUV + radiusUV) * float(resolution) - 0.5)) + 1;
    if (any(lessThan(lo, ivec2(0))) || any(greaterThanEqual(hi, ivec2(resolution))))
        return false;
    uvec2 lowPage = uvec2(lo) / kGE_VirtualShadowPageTexels;
    uvec2 highPage = uvec2(hi) / kGE_VirtualShadowPageTexels;
    if (any(greaterThan(highPage - lowPage, uvec2(1u))))
        return false;
    uint layerCount = uint(textureSize(ge_virtualShadowDepth, 0).z);
    for (uint y = 0u; y < 2u; ++y)
    {
        for (uint x = 0u; x < 2u; ++x)
        {
            uint layer = kGE_VirtualShadowMissingPage;
            if (lowPage.x + x <= highPage.x && lowPage.y + y <= highPage.y)
            {
                layer = GE_VirtualShadowResolveTile(firstPage, side, lowPage + uvec2(x, y), layerCount);
                if (layer == kGE_VirtualShadowMissingPage)
                    return false;
            }
            quad.Layers[y * 2u + x] = layer;
        }
    }
    quad.Resolution = float(resolution);
    quad.TexelOrigin = vec2(lowPage * kGE_VirtualShadowPageTexels);
    return true;
}

// relative is the texel position from the quad's texel origin.
float GE_VirtualShadowQuadTexelDepth(GE_VirtualShadowKernelQuad quad, ivec2 relative)
{
    ivec2 page = relative / int(kGE_VirtualShadowPageTexels);
    ivec2 withinPage = relative - page * int(kGE_VirtualShadowPageTexels);
    return texelFetch(ge_virtualShadowDepth, ivec3(withinPage, int(quad.Layers[page.y * 2 + page.x])), 0).r;
}

float GE_VirtualShadowQuadTexel(GE_VirtualShadowKernelQuad quad, ivec2 relative, float reference)
{
    return reference >= GE_VirtualShadowQuadTexelDepth(quad, relative) ? 1.0 : 0.0;
}

// The raw depth of the texel under uv. uv must lie inside the resolved kernel.
float GE_VirtualShadowQuadDepth(GE_VirtualShadowKernelQuad quad, vec2 uv)
{
    return GE_VirtualShadowQuadTexelDepth(quad, ivec2(floor(uv * quad.Resolution)) - ivec2(quad.TexelOrigin));
}

// uv must lie inside the kernel that GE_VirtualShadowResolveKernelQuad resolved.
float GE_VirtualShadowCompareKernelQuad(GE_VirtualShadowKernelQuad quad, vec2 uv, float reference)
{
    vec2 texel = uv * quad.Resolution - 0.5;
    ivec2 relative = ivec2(floor(texel)) - ivec2(quad.TexelOrigin);
    ivec2 page = relative / int(kGE_VirtualShadowPageTexels);
    ivec2 farPage = (relative + 1) / int(kGE_VirtualShadowPageTexels);
    if (all(equal(page, farPage)))
    {
        vec2 local = (texel - quad.TexelOrigin - vec2(page * int(kGE_VirtualShadowPageTexels)) + 0.5) /
                     float(kGE_VirtualShadowPageTexels);
#ifdef GE_VIRTUAL_SHADOW_HARDWARE_COMPARE
        return textureGrad(ge_virtualShadowDepthCompare,
                           vec4(local, float(quad.Layers[page.y * 2 + page.x]), reference), vec2(0.0), vec2(0.0));
#endif
    }
    vec2 weight = fract(texel);
    float c00 = GE_VirtualShadowQuadTexel(quad, relative, reference);
    float c10 = GE_VirtualShadowQuadTexel(quad, relative + ivec2(1, 0), reference);
    float c01 = GE_VirtualShadowQuadTexel(quad, relative + ivec2(0, 1), reference);
    float c11 = GE_VirtualShadowQuadTexel(quad, relative + ivec2(1, 1), reference);
    return mix(mix(c00, c10, weight.x), mix(c01, c11, weight.x), weight.y);
}

// projection = (first page, finest mip width in pages, last mip).
// Resolve each contributing hardware-PCF sub-tap through the virtual page
// table before interpolating the compare results. The caller can retry the
// complete filter at a coarser mip or use the conventional map on failure.
bool GE_VirtualShadowCompareBilinear(uvec3 projection, uint mip, vec2 uv, float reference,
                                    out float visible)
{
    visible = 0.0;
    uint baseSide = projection.y;
    if (baseSide == 0u || baseSide > 128u || (baseSide & (baseSide - 1u)) != 0u ||
        mip > projection.z || mip > uint(findMSB(baseSide)) ||
        any(isnan(uv)) || any(isinf(uv)) || isnan(reference) || isinf(reference))
        return false;

    uint firstPage = GE_VirtualShadowMipStart(projection.x, baseSide, mip);
    if (firstPage < projection.x)
        return false;
    uint side = baseSide >> mip;
    int resolution = int(side * kGE_VirtualShadowPageTexels);
    vec2 texel = clamp(uv, vec2(0.0), vec2(1.0)) * float(resolution) - 0.5;
    ivec2 lo = clamp(ivec2(floor(texel)), ivec2(0), ivec2(resolution - 1));
    ivec2 hi = clamp(ivec2(floor(texel)) + 1, ivec2(0), ivec2(resolution - 1));
    vec2 weight = fract(texel);
    if (hi.x == lo.x)
        weight.x = 0.0;
    if (hi.y == lo.y)
        weight.y = 0.0;

    uint layerCount = uint(textureSize(ge_virtualShadowDepth, 0).z);
    uvec2 lowPage = uvec2(lo) / kGE_VirtualShadowPageTexels;
    uvec2 highPage = uvec2(hi) / kGE_VirtualShadowPageTexels;
    if (all(equal(lowPage, highPage)) && all(equal(hi, lo + 1)))
    {
        uint tile = GE_VirtualShadowResolveTile(firstPage, side, lowPage, layerCount);
        if (tile == kGE_VirtualShadowMissingPage)
            return false;
#ifdef GE_VIRTUAL_SHADOW_HARDWARE_COMPARE
        vec2 localUV = (texel - vec2(lowPage * kGE_VirtualShadowPageTexels) + 0.5) / float(kGE_VirtualShadowPageTexels);
        visible = textureGrad(ge_virtualShadowDepthCompare, vec4(localUV, float(tile), reference), vec2(0.0), vec2(0.0));
#else
        // The footprint is wholly resident in one layer. Sample at the
        // exact midpoint of those four texels, then apply the original weights.
        vec2 center = vec2(lo % int(kGE_VirtualShadowPageTexels) + 1) / float(kGE_VirtualShadowPageTexels);
        vec4 depths = textureGather(ge_virtualShadowDepth, vec3(center, float(tile)));
        vec4 compare = vec4(greaterThanEqual(vec4(reference), depths));
        visible = mix(mix(compare.w, compare.z, weight.x), mix(compare.x, compare.y, weight.x), weight.y);
#endif
        return true;
    }

    float c00, c10, c01, c11;
    if (!GE_VirtualShadowCompareTexel(firstPage, side, lo, reference, layerCount, c00))
        return false;
    c10 = c00;
    if (weight.x > 0.0 &&
        !GE_VirtualShadowCompareTexel(firstPage, side, ivec2(hi.x, lo.y), reference, layerCount, c10))
        return false;
    c01 = c00;
    c11 = c10;
    if (weight.y > 0.0)
    {
        if (!GE_VirtualShadowCompareTexel(firstPage, side, ivec2(lo.x, hi.y), reference, layerCount, c01))
            return false;
        c11 = c01;
        if (weight.x > 0.0 &&
            !GE_VirtualShadowCompareTexel(firstPage, side, hi, reference, layerCount, c11))
            return false;
    }
    visible = mix(mix(c00, c10, weight.x), mix(c01, c11, weight.x), weight.y);
    return true;
}

#endif
