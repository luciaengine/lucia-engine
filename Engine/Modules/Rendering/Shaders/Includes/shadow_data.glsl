#ifndef GE_SHADOW_DATA_GLSL
#define GE_SHADOW_DATA_GLSL

// The directional shadow constants (ShadowDataGPU) at set-0 binding 8. The forward
// lighting and the virtual shadow sun mask pass read the same block.
layout(set = 0, binding = 8, std140) uniform ShadowData
{
    mat4 ge_shadowVP[4];   // Light view-projection per cascade
    vec4 ge_shadowSplits;  // View-space split distances (x=cascade0, y=cascade1, ...)
    vec4 ge_shadowParams;  // x=depthBias, y=normalBias, z=numCascades, w=maxShadowDistance
    vec4 ge_shadowDebug;   // x=debugMode, y=pcssActive (0=disabled, 1=PCSS on), z=frameIndex, w=pcfQuality
    // x = softness multiplier (Poisson PCF fallback only),
    // y = PCSS / Poisson disk tap count (runtime-bound loop, clamped to [8, 64]
    //     in the shader; the 8/16/32/64 set comes from the CPU setter,
    //     ShadowMapRenderFeature::SetPcssTapCount, not from this clamp),
    // z = maxPenumbraWorld (world-space cap, converted per-cascade in the shader),
    // w = receiverPlaneBias (0/1 toggle).
    vec4 ge_shadowPcss;
    // Per-cascade PCSS metrics (precomputed on CPU):
    //   x = world-per-texel (world units per shadow-map texel)
    //   y = depth span (world units, used to convert NDC depth delta to world delta)
    //   z = bindless index for this cascade's raw-depth texture (PCSS blocker search);
    //       unused under the compat profile, which reads ge_shadowMapRaw by layer
    //   w = effective light size for this cascade (= global * falloff^cascadeIdx)
    vec4 ge_shadowPcssCascades[4];
    // Per-cascade PCSS min/max pyramid (ShadowMinMaxPyramid, R = min, G = max):
    //   x = bindless index of this cascade's pyramid view; NEGATIVE means this
    //       frame has no pyramid and the PCSS filter runs unaccelerated
    //   y = mip levels in the pyramid
    //   z = base downshift — level 0 covers a (1 << z) square of shadow texels
    //   w = reserved
    vec4 ge_shadowPcssPyramid[4];
    // Frame-global filter params:
    //   x = dither basis (0 = screen space, 1 = shadow space) — ShadowDitherBasis
    //   y = authored distance fade fraction, zw = reserved (0)
    vec4 ge_shadowFilterParams;
    // The terrain's sun-space clearance map (terrain_shadow.glsl), ShadowDataGPU terrainShadow*:
    //   grid0: x, y = grid centre X, Z; z, w = terrain corner X, Z (render-origin relative)
    //   grid1: x, y = terrain extent X, Z; z, w = sun horizontal direction X, Z
    //   grid2: x = tan(elevation); y = texel (m); z, w = u of sample 0, v of line 0
    //   grid3: x = samples per line; y = lines; z = the terrain's base height (render-origin
    //          relative); w = metres per normalized height
    //   source: x = map bindless index; y = height texture bindless index; z = 1 when a map is
    //           published for this view this frame; w = map side (texels)
    //   field: x = the lit field's bindless index (0: none, TerrainShadowMap FieldLit); y = its levels
    //   heightSpan: x, y = the extent the height texture spans X, Z (whole tiles; grid1.xy is the
    //               authored size, the bound of "on the terrain")
    // Order matters more than naming: std140 lays this block out by position, matching
    // ShadowDataGPU member for member. A mismatch here is silent garbage, not a compile error.
    vec4 ge_terrainShadowGrid0;
    vec4 ge_terrainShadowGrid1;
    vec4 ge_terrainShadowGrid2;
    vec4 ge_terrainShadowGrid3;
    uvec4 ge_terrainShadowSource;
    vec4 ge_shadowReferenceDepth; // source-camera depth plane for mirror receivers
    uvec4 ge_terrainShadowField;
    vec4 ge_terrainShadowHeightSpan;
};

#endif
