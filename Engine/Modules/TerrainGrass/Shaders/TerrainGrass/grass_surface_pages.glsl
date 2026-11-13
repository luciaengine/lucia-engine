// The ground a blade stands on, for grass placement and grass shading: the surface pages, then the unified
// maps, then nothing, in the order the terrain fragment takes them. The includer supplies cbt_atlas.glsl,
// cbt_surface_pages.glsl and GRASS_SURFACE_SET / GRASS_SURFACE_PARAMS_BINDING (mirror GrassSurfaceParamsGPU).
#ifndef GRASS_SURFACE_PAGES_GLSL
#define GRASS_SURFACE_PAGES_GLSL

#ifndef CBT_ATLAS_GLSL
#error "grass_surface_pages.glsl needs cbt_atlas.glsl included first (CBT_LatticeTextureUV)"
#endif

// GE_SHARED_GRASS_GROUND_SOURCE_BEGIN
// TerrainGPUParams::Flags bit1 (mirror Terrain::kTerrainFlagAtlasBacked): the terrain has no unified
// height, normal or splat maps (HeightmapBindless == 0).
const uint kTerrainFlagAtlasBacked = 2u;

// Where a point's ground comes from: the pure decision placement and shading share, so the host suite
// executes this exact source.
const uint kGrassGroundNone = 0u;    // nothing bound: channel 0, the unwritten answer
const uint kGrassGroundPages = 1u;   // a surface page answers here
const uint kGrassGroundUnified = 2u; // the terrain-sized unified splatmap

uint GrassSurface_SelectSource(bool pagesResolved, uint splatmapBindless)
{
    if (pagesResolved)
        return kGrassGroundPages;
    if (splatmapBindless != 0u)
        return kGrassGroundUnified;
    return kGrassGroundNone;
}

// Whether the grass can read the ground at a point at all. An atlas-backed terrain keeps its ground in
// the terrain's atlas, which the grass does not read: off its pages no blade stands there.
bool GrassSurface_GroundReadable(uint terrainFlags, bool pagesResolved)
{
    return pagesResolved || (terrainFlags & kTerrainFlagAtlasBacked) == 0u;
}

#if !defined(GE_COMPAT_PROFILE)
// The ground fragment's footprint at view-space `posVS` (CBT_SurfPixelFootprint). The ground rises `slope`
// meters a meter along world +X and +Z; `axis*VS` are the world axes in view space, `projY` the projection's
// Y scale. `viewportHeight` is in pixels, negative for an orthographic view; 0 (size unknown) returns 0.
float GrassSurface_GroundPixelFootprint(vec3 posVS, vec3 axisXVS, vec3 axisYVS, vec3 axisZVS, vec2 slope,
                                        float projY, float viewportHeight)
{
    bool orthographic = viewportHeight < 0.0f;
    if (viewportHeight == 0.0f || (!orthographic && posVS.z <= 0.0f))
        return 0.0f;
    float pixelsPerNdc = projY * abs(viewportHeight) * 0.5f;
    // The ground's tangents along world X and Z, in view space.
    vec3 tangentX = axisXVS + axisYVS * slope.x;
    vec3 tangentZ = axisZVS + axisYVS * slope.y;
    // Pixels per meter of tangent: the columns of the screen Jacobian (no divide when orthographic).
    float z = orthographic ? 1.0f : posVS.z;
    float px = orthographic ? 0.0f : posVS.x;
    float py = orthographic ? 0.0f : posVS.y;
    float scale = pixelsPerNdc / (z * z);
    float jxX = (tangentX.x * z - px * tangentX.z) * scale;
    float jxY = (tangentX.y * z - py * tangentX.z) * scale;
    float jzX = (tangentZ.x * z - px * tangentZ.z) * scale;
    float jzY = (tangentZ.y * z - py * tangentZ.z) * scale;
    float det = jxX * jzY - jzX * jxY;
    if (abs(det) < 1e-12f)
        return 0.0f;
    // Its inverse: meters along X and Z per pixel step; the tangent plane gives the Y term.
    vec2 stepX = vec2(jzY, -jxY) * (1.0f / det);
    vec2 stepY = vec2(-jzX, jxX) * (1.0f / det);
    vec3 dPdx = vec3(stepX.x, stepX.x * slope.x + stepX.y * slope.y, stepX.y);
    vec3 dPdy = vec3(stepY.x, stepY.x * slope.x + stepY.y * slope.y, stepY.y);
    return CBT_SurfPixelFootprint(dPdx, dPdy);
}

// The smallest ground footprint of the four planes that `faceSlopes` make (the one-sided slopes to the
// -X, +X, -Z and +Z height sample). A triangle across a crest lies between these planes, so the placement
// never reads a coarser level than the ground. On a coarser level the grass weight spreads past its patch.
float GrassSurface_GroundPixelFootprintAcrossFaces(vec3 posVS, vec3 axisXVS, vec3 axisYVS, vec3 axisZVS,
                                                   vec4 faceSlopes, float projY, float viewportHeight)
{
    float footprint = GrassSurface_GroundPixelFootprint(posVS, axisXVS, axisYVS, axisZVS,
                                                        vec2(faceSlopes.x, faceSlopes.z), projY, viewportHeight);
    for (uint face = 1u; face < 4u; ++face)
    {
        vec2 slope = vec2((face & 1u) != 0u ? faceSlopes.y : faceSlopes.x,
                          (face & 2u) != 0u ? faceSlopes.w : faceSlopes.z);
        footprint = min(footprint, GrassSurface_GroundPixelFootprint(posVS, axisXVS, axisYVS, axisZVS, slope,
                                                                     projY, viewportHeight));
    }
    return footprint;
}
#endif
// GE_SHARED_GRASS_GROUND_SOURCE_END

vec4 grassNormalizeLayerWeights(vec4 weights)
{
    float wSum = weights.x + weights.y + weights.z + weights.w;
    if (wSum > 0.0)
        return weights / wSum;
    return vec4(1.0, 0.0, 0.0, 0.0);
}

// The unified splat tap at level 0 (a compute stage has no derivatives). The narrow arm, which has no
// bindless array, defines it with a named binding.
#ifndef GRASS_SPLAT_TAP_UNIFIED
#define GRASS_SPLAT_TAP_UNIFIED(bindlessIndex, uv) \
    textureLod(GE_BTEX(bindlessIndex, GE_TS_CLAMP), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(bindlessIndex, GE_TS_CLAMP), 0))), 0.0)
#endif

// The four normalized ground weights at `uv` where no page answers: the unified splatmap, or channel 0
// (the answer grassNormalizeLayerWeights gives an unwritten splat).
vec4 GrassSurface_UnifiedSplat(uint splatmapBindless, vec2 uv)
{
    if (GrassSurface_SelectSource(false, splatmapBindless) == kGrassGroundUnified)
        return grassNormalizeLayerWeights(GRASS_SPLAT_TAP_UNIFIED(splatmapBindless, uv));
    return grassNormalizeLayerWeights(vec4(0.0));
}

#if defined(CBT_SURFACE_PAGES_GLSL) && !defined(GE_COMPAT_PROFILE)
#if !defined(GRASS_SURFACE_SET) || !defined(GRASS_SURFACE_PARAMS_BINDING)
#error "grass_surface_pages.glsl needs GRASS_SURFACE_SET / _PARAMS_BINDING defined first"
#endif

// The paged terrain this frame (mirror TerrainGrassRenderFeature::GrassSurfaceParamsGPU, std140).
layout(std140, set = GRASS_SURFACE_SET, binding = GRASS_SURFACE_PARAMS_BINDING) uniform GrassSurfaceParamsBuffer
{
    uint PagedTerrainRow; // the terrains[] row whose height is paged; kGrassNoPagedTerrain when none
    uint SurfacePageBase; // the surface table's first word in gPageTable
    uint SurfacePlanes;   // the surface table's planes (CBT_SURF_PLANE_*); 0 when the surface is not paged
    uint Pad;
} Surface;

const uint kGrassNoPagedTerrain = 0xFFFFFFFFu;

// Whether terrains[row] draws its height from pages (the height table at word 0 of gPageTable).
bool GrassSurface_HeightPaged(uint row)
{
    return row == Surface.PagedTerrainRow;
}

// The paged height at `uv` in [0, 1] of the height range, at the finest resident level: the height the
// CBT displaces the terrain to (CBT_SampleHeight).
float GrassSurface_PagedHeight01(vec2 uv)
{
    return CBT_PageSample(uv.x, uv.y, 0.0, CBT_SurfPageField(0u));
}

// The surface pages of terrains[row] at `uv`, for a pixel `footprint` meters across: the terrain
// fragment's own resolve. Not resolved when the row's surface is not paged.
CBTSurfacePages GrassSurface_Pages(uint row, TerrainParamsEntry tp, vec2 uv, float footprint)
{
    if (row != Surface.PagedTerrainRow || Surface.SurfacePlanes == 0u)
        return CBT_SurfPagesNone();
    return CBT_SurfPagesAt(uv, footprint, tp.WorldSizeX, tp.WorldSizeZ, Surface.SurfacePageBase,
                           Surface.SurfacePlanes);
}
#endif // CBT_SURFACE_PAGES_GLSL && !GE_COMPAT_PROFILE

#endif // GRASS_SURFACE_PAGES_GLSL
