// The ground colour a blade grows out of: ONE colour per blade, resolved at its root.
//
// Included by terrain_grass_place.comp on the full profile, which resolves it once per placed
// blade and stores it in the blade instance (GrassBladeInstance::RootColor, packed by
// grass_clump.glsl). The vertex stage reads it from there and hands it to the surface through a
// FLAT varying, so the blade carries one ground colour from its root to its tip. A colour resolved
// at each fragment's own world position draws a textured ground's pattern up a leaning blade as
// streaks, because the blade's fragments stand over different ground.
//
// The resolve is the ground's own (Includes/terrain_material_albedo.glsl, the file cbt_surface.glsl
// shades the ground with), on the same ground the placement mask reads (grass_surface_pages.glsl),
// so the blade still takes the ground's color and not an approximation of it. What
// differs is the FOOTPRINT: the taps read the mip whose texel covers kGrassRootGroundFootprint of
// ground, so the root colour is the ground's local average around the blade, not the one texel
// under it. A blade a few centimetres wide that took the texel under its root would turn every
// fine detail of the ground into blade-to-blade noise.
//
// The terrain's colour map (TerrainColorMap.h) then mixes that average toward the painted colour by
// its weight, the expression the ground applies (cbt_surface.glsl), so a blade's base and the
// ground beside it agree under a stroke and under the grass's own written soil. The map modifies the
// footprint average; it does not replace it.
//
// The includer supplies, before this file: TerrainParamsEntry, the bindless texture table
// (Includes/bindless_textures.glsl), cbt_atlas.glsl, and GRASS_MATERIAL_TABLE_SET /
// GRASS_MATERIAL_TABLE_BINDING for the material table.

#ifndef GRASS_ROOT_GROUND_COLOR_GLSL
#define GRASS_ROOT_GROUND_COLOR_GLSL

// The blades of one placement workgroup can stand on different terrains, so a material's bindless
// index varies across invocations: nonuniform access (GE_BTEX).
#define CBT_LAYER_TEX(texIdx) GE_BTEX(texIdx, GE_TS_REPEAT)
#define CBT_LAYER_TEX_EDGE(texIdx) GE_BTEX(texIdx, GE_TS_CLAMP_ANISO)
// A compute stage has no implicit derivatives. The taps take explicit gradients, which
// GrassRootGroundColor sets to the root footprint, so every layer reads the mip whose texel spans
// that much ground at its own tiling. The per-view TAAU mip bias is a screen-space correction and
// does not apply to a footprint chosen in world metres, so the seam drops it.
#define CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias) textureGrad(CBT_LAYER_TEX(tex), uv, ddx, ddy).rgb
#define CBT_LAYER_TAP_EDGE(slot, tex, uv, ddx, ddy, bias) \
    textureGrad(CBT_LAYER_TEX_EDGE(tex), uv, ddx, ddy).rgb
#include "Includes/terrain_material_albedo.glsl"

// The terrain's material table, the buffer the ground's surface reads.
layout(std430, set = GRASS_MATERIAL_TABLE_SET, binding = GRASS_MATERIAL_TABLE_BINDING) readonly buffer
TerrainMaterialTableBuffer
{
    TerrainMaterialRecordData TerrainMaterials[];
};

#include "Includes/terrain_blend_width.glsl"
#include "Includes/terrain_blend_resolve.glsl"
#include "Includes/terrain_color_map.glsl"

// World metres of ground one root colour stands for. About the spacing of tufts (the shipped clump
// is 1.1 m), and coarse enough that a 2 m texture repeat resolves to a few texels.
const float kGrassRootGroundFootprint = 1.0;

// The ground at a blade's root the placement already resolved (grass_surface_pages.glsl): the four
// normalized weights, and on pages the page's normal and color planes.
struct GrassRootGround
{
    vec4 Weights;
    bool Paged;
    vec2 PagedNormalXZ;
    vec4 PagedColor;
};

// The terrain's own surface normal at the root, for the triplanar projection weights: the page's
// normal plane, else the unified normalmap, as the ground reads it there.
//
// Called only when a contributing material is textured: an untextured record resolves to its tint
// and never reads the projection weights.
vec3 grassRootTerrainNormalWS(TerrainParamsEntry tp, vec2 uv, GrassRootGround ground)
{
    vec2 nXZ = ground.PagedNormalXZ;
    if (!ground.Paged)
    {
        if (tp.NormalmapBindless == 0u)
            return vec3(0.0, 1.0, 0.0);
        nXZ = textureLod(GE_BTEX(tp.NormalmapBindless, GE_TS_CLAMP),
                         CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(tp.NormalmapBindless, GE_TS_CLAMP), 0))),
                         0.0).rg;
    }
    return normalize(vec3(nXZ.x, sqrt(max(1.0 - dot(nXZ, nXZ), 0.0)), nXZ.y));
}

// The color map at the root, read where the ground reads it: the page's color plane, else the
// unified map. A premultiplied texel filters to the right mix; vec4(0) is identity.
vec4 grassRootColorMap(TerrainParamsEntry tp, vec2 uv, GrassRootGround ground)
{
    if (ground.Paged)
        return ground.PagedColor;
    if (tp.ColorMapBindless == 0u)
        return vec4(0.0);
    return textureLod(GE_BTEX(tp.ColorMapBindless, GE_TS_CLAMP),
                      CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(tp.ColorMapBindless, GE_TS_CLAMP), 0))),
                      0.0);
}

// The terrain's resolved albedo around a blade's root, with the colour map applied. rootRelWS is
// the root relative to the render origin, which the projection UVs are anchored to.
vec3 GrassRootGroundColor(TerrainParamsEntry tp, vec3 rootWS, vec3 rootRelWS, GrassRootGround ground)
{
    vec2 uv = clamp(vec2((rootWS.x - tp.WorldOriginX) / max(tp.WorldSizeX, 0.0001),
                         (rootWS.z - tp.WorldOriginZ) / max(tp.WorldSizeZ, 0.0001)),
                    vec2(0.0), vec2(1.0));

    uint roles[4] = uint[4](tp.LayerRole[0], tp.LayerRole[1], tp.LayerRole[2], tp.LayerRole[3]);
    vec4 weights = CBT_ResolveBlendWeights(ground.Weights, roles);

    bool anyTextured = false;
    for (int i = 0; i < 4; ++i)
        anyTextured = anyTextured || (weights[i] > 0.0 && TerrainMaterials[roles[i]].AlbedoTex != 0u);
    CBTTriplanarCtx tc = CBT_BuildTriplanarCtx(
        rootRelWS, anyTextured ? grassRootTerrainNormalWS(tp, uv, ground) : vec3(0.0, 1.0, 0.0), uv);
    // The footprint as screen-space-style gradients: x along world X, y along world Y and Z, so
    // each of the three projections (XY, XZ, YZ) sees the footprint on both of its axes.
    float footprint = kGrassRootGroundFootprint;
    tc.dposdx = vec3(footprint, 0.0, 0.0);
    tc.dposdy = vec3(0.0, footprint, footprint);
    tc.dUVdx = vec2(footprint / max(tp.WorldSizeX, 0.0001), 0.0);
    tc.dUVdy = vec2(0.0, footprint / max(tp.WorldSizeZ, 0.0001));
    float uvTiling = CBT_MaterialUVTiling(tp.MaterialTiling);

    vec3 color = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        if (weights[i] > 0.0)
        {
            TerrainMaterialRecordData mat = TerrainMaterials[roles[i]];
            CBTMaterialUV muv = CBT_MaterialUVs(mat, tc, uvTiling);
            color += CBT_MaterialAlbedo(uint(i), mat, tc, muv, rootWS, footprint) * weights[i];
        }
    }
    return TerrainApplyColorMap(color, grassRootColorMap(tp, uv, ground));
}

#endif // GRASS_ROOT_GROUND_COLOR_GLSL
