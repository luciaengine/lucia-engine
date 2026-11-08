// The vertex modifier reads evaluated positions/UVs from CBT storage buffers; no vertex buffer is bound.
// Each three vertex indices address one visible bisector and its three corners.
// Set 2 binds those streams plus the draw-level/height chain; far planar corners filter by distance, near corners retain cached bits.
#define CBT_GRAPHICS_INCLUDE
#define CBT_DESCRIPTOR_SET 2
#include "cbt_layout.glsl"

#if defined(CBT_DRAW_HEIGHT_CHAIN)
// Choose the corner height from its distance-filtered chain level, or retain its cached Y if disabled or mismatched.
// The rule depends only on shared UV, cached position and frame eye, so neighboring facets share one height.
float CBT_DrawnCornerHeight(vec3 pos, vec2 uv)
{
    CBTDrawLevelParamsData p = gCbtDrawLevel.Params;
    if (!(p.eye.w > 0.0) || gCbtChain.Words[0] != p.lattice.x)
        return pos.y;
    precise float d = distance(pos, p.eye.xyz);
    float lambda = CBT_ChainLevel(d, p.eye.w);
    return CBT_ChainDrawnHeight(uv.x, uv.y, pos.y, lambda, p.lattice.x & 0xFFFFu, p.lattice.x >> 16u,
                                gCbtChain.Words[1], p.height.x, p.height.y);
}
#endif

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    uint vid = uint(gl_VertexIndex);
    uint triangle = vid / 3u;
    uint corner = vid - triangle * 3u;

    uint slot = gIdxVis.IndicesVisible[triangle];
    CBTVertexData vd = gVertex.Vertex[slot];

    // This vertex's position + terrain UV (corner.w = uv.x, meta[k] = uv.y). The compute
    // VertexEval already displaced the position (height baked in). The per-pixel normal is
    // sampled from the terrain normalmap in the surface (meta.w = its bindless index),
    // matching CDLOD — smooth + crack-free (plan §8 C4).
    vec3 pos;
    vec2 uv;
    uvec2 packedSector;
    if (corner == 0u)      { pos = vd.corner0.xyz; uv = vec2(vd.corner0.w, vd.meta.x); packedSector = vd.sector0; }
    else if (corner == 1u) { pos = vd.corner1.xyz; uv = vec2(vd.corner1.w, vd.meta.y); packedSector = vd.sector1; }
    else                   { pos = vd.corner2.xyz; uv = vec2(vd.corner2.w, vd.meta.z); packedSector = vd.sector2; }

    // Storage mode rides the per-slot deepTag (terrain S2a). Legacy slots (0): pos is the
    // fp32 WORLD position and the adapter takes the GE_ClipFromWorld path — byte-identical
    // to the pre-S2a draw (dark-ship). Deep slots (1): pos is the SECTOR-LOCAL offset and
    // this corner's integer world sector rides v.sector, so the adapter projects through
    // GE_ClipFromSectorLocal — the exact-integer-delta reconstruction that keeps Earth-
    // magnitude geometry fp32-precise in clip space.
    v.position = pos;
    if (vd.deepTag.x != 0u)
    {
        v.sector = CBT_UnpackCornerSector(packedSector);
        v.useSector = true;
    }
#if defined(CBT_DRAW_HEIGHT_CHAIN)
    else
    {
        v.position.y = CBT_DrawnCornerHeight(pos, uv);
    }
#endif
    v.normal = vec3(0.0, 1.0, 0.0);              // placeholder; the surface samples the normalmap
    v.uv0 = uv;                                  // terrain UV: normalmap + future splat lookups
    v.custom0 = vec4(float(slot), 0.0, 0.0, vd.meta.w); // .x = slot (debug), .w = normalmap index
}
