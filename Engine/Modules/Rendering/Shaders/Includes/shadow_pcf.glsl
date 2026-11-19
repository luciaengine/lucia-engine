#ifndef GE_SHADOW_PCF_GLSL
#define GE_SHADOW_PCF_GLSL

// The 5x5 PCF kernel over a cascade array. Define GE_SHADOW_PCF_MAP as the sampler2DArrayShadow
// before this include: the forward pass constructs it from a separate image and a shared compare
// sampler, and a constructed sampler cannot be a function argument.
#ifndef GE_SHADOW_PCF_MAP
#error "define GE_SHADOW_PCF_MAP as the cascade array's sampler2DArrayShadow before including shadow_pcf.glsl"
#endif

#include "shadow_filter.glsl"

float GE_ShadowPcf5x5(vec2 uv, float layer, float depth, vec2 receiverGradient)
{
    vec2 size = vec2(textureSize(GE_SHADOW_PCF_MAP, 0).xy);
    vec2 texelPosition = uv * size - 0.5;
    vec2 base = floor(texelPosition);
    vec3 offsetsX, offsetsY, weightsX, weightsY;
    GE_ShadowPcfAxis(fract(texelPosition.x), offsetsX, weightsX);
    GE_ShadowPcfAxis(fract(texelPosition.y), offsetsY, weightsY);

    float visibility = 0.0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
        {
            vec2 sampleUV = (base + vec2(offsetsX[x], offsetsY[y]) + 0.5) / size;
            // ShadowComparePCF supplies the reverse-Z lit border (depth zero).
            // Let the sampler blend border texels too: rejecting the whole
            // bilinear sample at the UV boundary would change the kernel.
            visibility += weightsX[x] * weightsY[y] *
                GE_SHADOW_TAP(GE_SHADOW_PCF_MAP, vec4(sampleUV, layer, depth +
                    dot(sampleUV - uv, receiverGradient) + dot(abs(receiverGradient), 1.0 / size)));
        }
    return visibility / 256.0;
}

#endif
