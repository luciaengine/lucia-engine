// Colour-keyed text coverage correction: the blend Chrome draws text with on
// macOS, carried into whatever space the attachment blends in.
//
// Glyph coverage out of the Slug rasteriser is the exact area of the dilated
// outline (slug_functions.glsl). What remains is how that coverage becomes a
// blend weight, split into two independent terms.
//
// 1. Contrast, keyed on the paint colour. Chrome 155 on macOS 15.7 (headless,
//    DPR 1 and 2, measured on straight Roboto stems from 8 to 45 px) blends
//    text as an sRGB-ENCODED source-over whose weight is
//        a = c + k c (1 - c)
//    (Skia's apply_contrast shape), with k set by the paint's encoded
//    luminance in four levels: the measured fit is
//    kTextContrastByLuminance below, from slightly thinning near-black text
//    to a strong bulge for near-white text. Light text on a dark ground is
//    the heavy polarity there, the reverse of Skia's own
//    contrast * linDst keying, and the four levels are Chrome's: the
//    background never enters (white on #404040 measured identical to white on
//    black). These levels apply to a desired encoded blend target. A desired
//    linear target uses Skia's boost keyed on the guessed linear backdrop.
//    `contrast` scales that target's k; 0 is plain coverage.
//
// 2. Blend-space retarget, bidirectional. An attachment interpolates either
//    LINEAR values (a linear intermediate; the terminal encode owns the OETF)
//    or raw sRGB-ENCODED bytes (the browser blend model: source-over on UNORM
//    bytes holding encoded values). The retarget solves, per fragment, for the
//    coverage whose blend in the attachment's ACTUAL space (blendSpaceEncoded)
//    lands where a blend in the TARGET space (blendGamma) would have put it:
//
//      linear  attachment, linear  target  ->  identity
//      linear  attachment, encoded target  ->  the encoded-blend look out of
//                                              a linear compositor
//      encoded attachment, linear  target  ->  the pre-distortion that makes
//                                              an encoded source-over land
//                                              area-exact
//      encoded attachment, encoded target  ->  identity, by the same maths
//
//    The term 1 weight is an encoded-blend weight. The editor targets the
//    encoded space (blendGamma 1.0) on its EncodedSrgb attachment, where the
//    retarget is the identity and the result is Chrome's. A UIManager's
//    default is the linear target, the identity on a linear attachment. The
//    retarget guesses the destination as the paint's perceptual inverse
//    (dst = 1 - src), which is what lets it run without a framebuffer read.
//
// Both terms are keyed on the glyph's colour and never on ppem: identical
// glyph shapes must not gain or lose ink for having been asked for at a
// different size (the size-keyed part of Chrome's look is the dilation).

#ifndef UI_TEXT_MASK_GAMMA_GLSL
#define UI_TEXT_MASK_GAMMA_GLSL

const vec3 kRec709Luma = vec3(0.2126, 0.7152, 0.0722);

// blendGamma sentinel: the target blend space is sRGB-linear. For a linear
// attachment the retarget is then identity and the contrast term is the whole
// correction; for an encoded attachment it selects the Skia-direction
// correction (area-exact ink out of an encoded blend).
const float kTextBlendGammaLinearTarget = 0.0;

// Below this |srcLin - dstLin| the linear-arm inversion is 0/0. Skia contains
// the same instability with a 1/256 guard on |src - dst|; this is that guard
// carried into linear units, where the encoded midpoint's slope is ~0.64.
const float kTextSpanFadeEnd = 1.0 / 128.0;

// The encoded-arm twin of kTextSpanFadeEnd: same 2x-Skia-guard fade width, in
// the units this arm divides in — encoded — where the 1/256 guard applies
// natively, no slope carry. Both arms are unstable at the same glyph colour
// (mid-grey against its own inverse, src = 0.5 encoded).
const float kTextSpanFadeEndEncoded = 1.0 / 128.0;

float srgbEncodeScalar(float linear)
{
    float c = clamp(linear, 0.0, 1.0);
    return (c <= 0.0031308) ? (c * 12.92) : (1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float srgbDecodeScalar(float encoded)
{
    float c = clamp(encoded, 0.0, 1.0);
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

// Skia's apply_contrast(). Fixes both endpoints and bulges the middle (or
// sags it, for a negative contrast), so it moves weight on partially covered
// pixels without leaking outside the glyph. Stays in [0, 1] for contrast in
// [-1, 1].
float applyTextContrast(float srca, float adjustedContrast)
{
    return srca + (1.0 - srca) * adjustedContrast * srca;
}

// Chrome's macOS contrast k per paint luminance level. Chrome keys it on the
// top two bits of the 8-bit encoded luminance: measured with greys in steps of
// one, 63 | 64, 127 | 128 and 191 | 192 change level and no other neighbours
// do. Fitted jointly with the dilation in ui_sdf_common.glsl (rms 0.003 to
// 0.009 of full ink per level).
const vec4 kTextContrastByLuminance = vec4(-0.14, 0.20, 0.53, 0.85);

// The level edges, halfway between the two 8-bit levels either side.
const vec3 kTextContrastLevelEdges = vec3(63.5, 127.5, 191.5) / 255.0;

// Half-width of the ramp that joins two levels at an edge: half an 8-bit
// level, so k is continuous in luminance while every 8-bit grey, including
// the two either side of an edge, keeps Chrome's exact level. The editor's
// most used label greys (#bdbdbd to #bfbfbf) sit one to two levels below the
// 191 | 192 edge, so a wider ramp would move them off Chrome.
const float kTextContrastEdgeHalfWidth = 0.5 / 255.0;

// The contrast for an encoded paint luminance in [0, 1].
float textChromeContrast(float srcEncoded)
{
    float k = kTextContrastByLuminance[0];
    for (int edge = 0; edge < 3; ++edge)
    {
        float t = smoothstep(kTextContrastLevelEdges[edge] - kTextContrastEdgeHalfWidth,
                             kTextContrastLevelEdges[edge] + kTextContrastEdgeHalfWidth, srcEncoded);
        k = mix(k, kTextContrastByLuminance[edge + 1], t);
    }
    return k;
}

// The correction is keyed on the glyph paint's LINEAR luminance in every
// pipeline variant; the paint arrives in the variant's BLEND space (linear by
// default, raw sRGB bytes under UI_BLEND_SPACE_ENCODED), so the encoded
// variant decodes here — for the KEY only. Decoding per channel before the
// luma dot keeps the key bit-comparable across variants: the same glyph
// colour selects the same correction whichever space it blends in.
float textGlyphLinearLuma(vec3 glyphPaint)
{
#ifdef UI_BLEND_SPACE_ENCODED
    return dot(vec3(srgbDecodeScalar(glyphPaint.r), srgbDecodeScalar(glyphPaint.g),
                    srgbDecodeScalar(glyphPaint.b)),
               kRec709Luma);
#else
    return dot(clamp(glyphPaint, vec3(0.0), vec3(1.0)), kRec709Luma);
#endif
}

// Where the TARGET-space blend of boosted coverage `a` lands, in linear light.
// src/dst are the guessed encoded endpoints, srcLin/dstLin their linear twins.
float textTargetBlendLinear(float a, float src, float dst, float srcLin, float dstLin,
                            float blendGamma)
{
    if (blendGamma <= kTextBlendGammaLinearTarget)
        return mix(dstLin, srcLin, a);
    float srcG = pow(src, blendGamma);
    float dstG = pow(dst, blendGamma);
    return srgbDecodeScalar(pow(mix(dstG, srcG, a), 1.0 / blendGamma));
}

// coverage           area-exact glyph coverage.
// glyphPaint         the glyph's rgb in the variant's BLEND space (see
//                    textGlyphLinearLuma).
// contrast           scale on term 1: Chrome's measured levels for an encoded
//                    target, Skia's backdrop boost for a linear target; 0 disables.
// blendGamma         kTextBlendGammaLinearTarget, or the exponent of the
//                    TARGET space the coverage should blend linearly in
//                    (1.0 = display-encoded).
// blendSpaceEncoded  0: the attachment interpolates LINEAR values.
//                    1: the attachment interpolates raw sRGB-ENCODED bytes.
float correctTextCoverage(float coverage, vec3 glyphPaint, float contrast, float blendGamma,
                          int blendSpaceEncoded)
{
    // srcLin is the linear level the correction models; src is its
    // display-encoded level, which is what the contrast level and the
    // destination guess below are keyed on.
    float srcLin = textGlyphLinearLuma(glyphPaint);
    float src = srgbEncodeScalar(srcLin);
    float dst = 1.0 - src;
    float dstLin = srgbDecodeScalar(dst);

    // The measured Chrome levels apply to an encoded blend target. A desired
    // linear blend keeps Skia's backdrop-keyed boost, preserving HUD weight.
    float k = (blendGamma <= kTextBlendGammaLinearTarget) ? dstLin : textChromeContrast(src);
    float a = applyTextContrast(coverage, contrast * k);

    if (blendSpaceEncoded == 0)
    {
        if (blendGamma <= kTextBlendGammaLinearTarget)
            return a;

        // Retarget: solve for the coverage whose LINEAR blend lands where a
        // blend in the blendGamma space would have put it.
        float wantedLin = textTargetBlendLinear(a, src, dst, srcLin, dstLin, blendGamma);
        float span = srcLin - dstLin;
        // span -> 0 is mid-grey text against its own inverse; the inversion's
        // limit there is `a`, so fade to that rather than dividing by nothing.
        // The guard must sit on the DENOMINATOR: at span == 0 the quotient is
        // 0/0, and NaN survives a zero mix weight. Any nonzero span clamps
        // finite, so exact zero is the one case to reroute.
        float safeSpan = (span == 0.0) ? 1.0 : span;
        return mix(a, clamp((wantedLin - dstLin) / safeSpan, 0.0, 1.0),
                   smoothstep(0.0, kTextSpanFadeEnd, abs(span)));
    }

    // Skia-direction arm: solve for the coverage whose ENCODED source-over
    // lands where a blend in the blendGamma space would have put it. At the
    // linear-target sentinel this is the full area-exact correction; at
    // blendGamma 1.0 (the encoded space itself) it is the identity — each
    // arm's identity sits at its own blend space, which is what makes the
    // retarget bidirectional rather than a pair of special cases.
    float wantedLin = textTargetBlendLinear(a, src, dst, srcLin, dstLin, blendGamma);
    float spanEnc = src - dst;
    // Same denominator guard as the linear arm: 0/0 at the exact inverse
    // poisons the mix even at weight zero.
    float safeSpanEnc = (spanEnc == 0.0) ? 1.0 : spanEnc;
    return mix(a, clamp((srgbEncodeScalar(wantedLin) - dst) / safeSpanEnc, 0.0, 1.0),
               smoothstep(0.0, kTextSpanFadeEndEncoded, abs(spanEnc)));
}

#ifdef UI_SUBPIXEL_DUAL_SRC
// Per-channel form for subpixel RGB coverage: one correction per channel, all
// keyed on the SAME paint luminance — subpixel AA moves where coverage is
// sampled, not what colour the correction believes it is drawing. A per-channel
// key would re-weight the R/G/B fringes against each other and tint the glyph.
vec3 correctTextCoverageRGB(vec3 coverage, vec3 glyphPaint, float contrast, float blendGamma,
                            int blendSpaceEncoded)
{
    return vec3(correctTextCoverage(coverage.r, glyphPaint, contrast, blendGamma, blendSpaceEncoded),
                correctTextCoverage(coverage.g, glyphPaint, contrast, blendGamma, blendSpaceEncoded),
                correctTextCoverage(coverage.b, glyphPaint, contrast, blendGamma, blendSpaceEncoded));
}
#endif // UI_SUBPIXEL_DUAL_SRC

#endif // UI_TEXT_MASK_GAMMA_GLSL
