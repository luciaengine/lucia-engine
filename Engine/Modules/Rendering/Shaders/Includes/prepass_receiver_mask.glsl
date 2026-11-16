#ifndef GE_PREPASS_RECEIVER_MASK_GLSL
#define GE_PREPASS_RECEIVER_MASK_GLSL

// One R32_UINT texel of a screen-space mask that a compute pass writes for the
// prepass receiver of its pixel. The high 24 bits keep the receiver depth: the
// sign, the exponent and the upper 15 mantissa bits (relative truncation error
// <= 3.052e-5). The low byte keeps the value of the pass. A texel with a zero
// depth has no receiver.

uint GE_PackPrepassReceiver(float receiverDepth, uint value)
{
    return (floatBitsToUint(receiverDepth) & 0xffffff00u) | (value & 255u);
}

uint GE_PrepassReceiverValue(uint packed)
{
    return packed & 255u;
}

// True when the fragment at fragmentDepth is the receiver that the texel holds.
// This rejects the phase-B recovered surfaces, the transparent layers and the
// forward-only surfaces at another depth. The resolved MSAA depth comes from a
// sample in the pixel, and the fragment can evaluate its depth at the center:
// footprint is fwidth(gl_FragCoord.z), and half of it is allowed on top of the
// packed-depth error, so a sloped receiver still matches.
bool GE_IsPrepassReceiver(uint packed, float fragmentDepth, float footprint)
{
    float receiverDepth = uintBitsToFloat(packed & 0xffffff00u);
    float tolerance = max(abs(fragmentDepth) * 3.1e-5, 1e-12) + 0.5 * footprint;
    return receiverDepth > 0.0 && abs(receiverDepth - fragmentDepth) <= tolerance;
}

#endif
