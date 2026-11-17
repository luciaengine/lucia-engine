#ifndef GE_PREPASS_RECEIVER_MASK_GLSL
#define GE_PREPASS_RECEIVER_MASK_GLSL

// An R32_UINT screen-space mask texel for the prepass receiver of its pixel. High 24 bits: the
// receiver depth (relative truncation error <= 3.052e-5). Low byte: the value of the pass.
// A zero depth means no receiver.

uint GE_PackPrepassReceiver(float receiverDepth, uint value)
{
    return (floatBitsToUint(receiverDepth) & 0xffffff00u) | (value & 255u);
}

uint GE_PrepassReceiverValue(uint packed)
{
    return packed & 255u;
}

// fwidth(gl_FragCoord.z) from its gradient (dFdx, dFdy).
float GE_PrepassReceiverFootprint(vec2 depthGradient)
{
    return abs(depthGradient.x) + abs(depthGradient.y);
}

// True when the fragment is the receiver of the texel; other surfaces at the pixel are rejected.
// footprint = fwidth(gl_FragCoord.z): half of it is allowed, because the resolved MSAA depth is a
// sample in the pixel and the fragment depth is at its center.
bool GE_IsPrepassReceiver(uint packed, float fragmentDepth, float footprint)
{
    float receiverDepth = uintBitsToFloat(packed & 0xffffff00u);
    float tolerance = max(abs(fragmentDepth) * 3.1e-5, 1e-12) + 0.5 * footprint;
    return receiverDepth > 0.0 && abs(receiverDepth - fragmentDepth) <= tolerance;
}

#endif
