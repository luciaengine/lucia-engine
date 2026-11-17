#ifndef GE_SHADOW_PCSS_TAPS_GLSL
#define GE_SHADOW_PCSS_TAPS_GLSL

// The Vogel disk rotation and tap count of the directional PCSS kernel. Needs the
// ShadowData block (shadow_data.glsl).
#include "shadow_data.glsl"

// Compile-time upper bound on the runtime-variable tap count. Drivers need a
// constant loop bound for clean SPIR-V codegen; the actual iteration count is
// capped at runtime via `tapCount` (clamped to [8, 64] from ge_shadowPcss.y).
const int kPcssMaxTaps = 64;

#include "vogel_disk.glsl"

// Per-pixel pseudo-random rotation for the Vogel disk. Returns (cosA, sinA).
//
// Interleaved gradient noise (Jimenez 2014) over a basis chosen at runtime by
// ge_shadowFilterParams.x:
//
//   Screen (0)      keys on gl_FragCoord. The noise field is nailed to the
//                   SCREEN, so geometry slides under it as the camera moves —
//                   the grain swims. High spatial frequency, which is what the
//                   other basis gives up.
//   ShadowSpace (1) keys on the shadow coordinate, welding the field to the
//                   shadow lattice (and via the centre texel snap, to the
//                   world). Under magnification adjacent screen pixels map to
//                   nearby shadow coords and so decorrelate LESS: the noise
//                   drops in frequency, reading blotchy rather than grainy.
//
// Neither is correct in general — the trade is range-dependent — so this is a
// live toggle rather than a fix, and Screen stays the default because it is
// what shipped.
//
// The shadow key is the CONTINUOUS product, never floor()ed. Flooring gives
// every screen pixel inside one shadow texel the same angle, which under
// magnification turns fine grain into texel-sized blocks — the exact artifact
// this basis is meant to avoid. (The local families in clustered_lighting.glsl
// do floor theirs; that is a known divergence, not a pattern to copy.)
vec2 GE_PoissonRotation(vec2 screenPos, vec2 shadowUV, vec2 texSize)
{
    vec2 key = (ge_shadowFilterParams.x > 0.5) ? (shadowUV * texSize) : screenPos;
    float angle = fract(52.9829189 * fract(dot(key, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    return vec2(cos(angle), sin(angle));
}

// Runtime tap count. This clamps the ENDS only and does not snap to the allowed
// set: ShadowMapRenderFeature::SetPcssTapCount rounds to the nearest of
// {8, 16, 32, 64} before upload, so the set is a CPU-side guarantee.
int GE_PcssTapCount()
{
    int n = int(ge_shadowPcss.y + 0.5);
    return clamp(n, 8, kPcssMaxTaps);
}

#endif
