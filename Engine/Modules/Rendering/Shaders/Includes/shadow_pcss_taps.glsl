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

// Kernel width, in texels, from which the tap count grows past the authored one.
const float kPcssTapRefTexels = 16.0;

// Sample count scaled to how many texels the kernel actually spans.
//
// Once the penumbra is a fixed WORLD size, its width in texels varies hugely by
// cascade: a fine-texel near cascade spans many texels for the same physical
// penumbra, a coarse far cascade spans few. One global tap count therefore
// under-samples one end (banding) and wastes work at the other. The authored
// count (ge_shadowPcss.y) stays the dial — it is the count at
// kPcssTapRefTexels, so raising it shifts the whole curve rather than pinning
// a single width.
//
// Linear in radius, not quadratic: constant sample DENSITY over a disk would
// need taps proportional to area, which explodes. Linear keeps the cost bounded
// and kPcssMaxTaps bounds it absolutely.
//
// The authored count is the floor: a kernel narrower than kPcssTapRefTexels keeps
// it rather than scaling down. A physical sun's penumbra is a few texels wide (a
// 0.53 degree sun over a 2 m composition: about 4.6 texels), and scaled down that
// kernel fell to 8 taps, too few for the per-pixel rotated disk to read as
// anything but stipple. Taps lie in [authored, kPcssMaxTaps].
int GE_PcssTapsForRadius(float radiusTexels)
{
    int authored = GE_PcssTapCount();
    float scaled = float(authored) * radiusTexels / kPcssTapRefTexels;
    return clamp(int(scaled + 0.5), authored, kPcssMaxTaps);
}

// Blocker-search bounds, in SHADOW-MAP TEXELS — a sampling-density budget, not
// a physical size. The physical size comes from the light's angular diameter:
// searchWorld = tan(halfAngle) * searchDepthWorld. Because searchDepthWorld is a
// fraction of the cascade's depth span, and depth span and worldPerTexel both
// scale with cascade extent, the resulting texel radius is near-constant across
// cascades — measured 16.6 vs 16.0 texels per unit fraction for cascades 0 and 3
// at the 100 m / 4-cascade / 2048 default. That is the cascade invariance a
// world-space clamp was reaching for and could not deliver, because the value it
// clamped was a dimensionless tangent rather than a length.
//
// Min 2.0: below this every Vogel tap can still resolve into a single texel under
// bilinear snapping, making avgBlocker a point sample instead of an average — and
// a point sample that misses sizes the minimum kernel.
const float kPcssMinSearchTexels = 2.0;
// Max scales with sqrt(tapCount) so sample density stays roughly constant over
// the runtime 8..64 tap range; a fixed ceiling either starves 64-tap quality or
// lets 8-tap dither.
const float kPcssSearchTexelsPerRootTap = 4.0;
// Fraction of the cascade's world-space depth span the blocker search spans:
// how far behind the receiver an occluder may sit and still be found. Larger =
// softer far field, but more risk of distant occluders inflating avgBlocker into
// a halo at depth discontinuities. The resulting radius is then bounded by the
// widest PCF kernel (GE_PcssBoundedSearchTexels), so for the shipped sun the
// kernel cap, not this fraction, usually sets the searched width.
const float kPcssSearchDepthFraction = 2.0;

// Auto ceiling on the physical penumbra, in WORLD units and uniform across
// cascades. This is the whole point of the slice: the previous ceiling was
// 4*sqrt(taps) * worldPerTexel, so halving shadow resolution doubled the
// permitted penumbra in metres while the tap count stayed put. Measured on
// ShadowStress with the cascade fit held identical: median edge-width ratio
// 2.12 across 12 paired samples, 2048 vs 1024.
//
// It rarely binds for a real sun -- at 0.53 degrees a 0.5 m penumbra needs a
// 108 m caster-receiver gap -- so for the shipped light the penumbra stays
// purely physical and this is inert. It exists for wide lights, where it is the
// primary look knob; ge_shadowPcss.z (maxPenumbra, 0 = auto) overrides it per
// project. Mirrored on the CPU by kPcssMaxPenumbraWorldAuto in
// ShadowMapRenderFeature.h, which sizes the min/max pyramid for this cap.
const float kPcssMaxPenumbraWorldAuto = 0.5;

#endif
