// The display-weighted mean of a pixel's samples, so a bright surface that covers one sample in eight gets one
// eighth of its displayed light. The includer binds the Aces2Tables buffer (display_response.glsl).
#ifndef GE_MSAA_DISPLAY_RESOLVE_GLSL
#define GE_MSAA_DISPLAY_RESOLVE_GLSL

#include "hdr_sanitize.glsl"
#include "display_response.glsl"
#include "karis_weight.glsl"

// The display-weighted mean. Sample i shows light Y_i = T(scale * L_i): T is the look's response, scale is
// the exposure / the pre-exposure. The output luma is T^-1(mean Y_i) / scale in the samples' luma range, and
// c_i / L_i is weighted by Y_i. Alpha, `plain` and a pixel with no light or one luma use the plain mean.
void GE_MsaaDisplayResolve(sampler2DMS samples, ivec2 pixel, int look, float scale, out vec4 resolved,
                           out vec4 plain)
{
    int count = textureSamples(samples);
    vec4 plainSum = vec4(0.0);
    vec3 chromaSum = vec3(0.0);
    float lightSum = 0.0;
    float lumaMin = kMaxHDR;
    float lumaMax = 0.0;
    for (int i = 0; i < count; ++i)
    {
        vec4 sampleColor = texelFetch(samples, pixel, i);
        sampleColor.rgb = Sanitize(sampleColor.rgb);
        float luma = GE_KarisLuma(sampleColor.rgb);
        float light = GE_DisplayResponse(look, scale * luma);
        // A sample with zero luma is black and shows no light, so its term is zero.
        chromaSum += sampleColor.rgb * (light / max(luma, 1e-20));
        lightSum += light;
        plainSum += sampleColor;
        lumaMin = min(lumaMin, luma);
        lumaMax = max(lumaMax, luma);
    }
    plain = plainSum / float(count);
    // Samples of one luma have equal weights, so their weighted mean is the plain mean, exactly.
    if (lightSum <= 0.0 || lumaMax <= lumaMin)
    {
        resolved = plain;
        return;
    }
    float luma = clamp(GE_DisplayResponseInverse(look, lightSum / float(count)) / scale, lumaMin, lumaMax);
    // chromaSum / lightSum has luma 1.
    resolved = vec4(chromaSum * (luma / lightSum), plain.a);
}

#endif // GE_MSAA_DISPLAY_RESOLVE_GLSL
