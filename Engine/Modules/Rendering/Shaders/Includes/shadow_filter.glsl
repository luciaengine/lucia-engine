#ifndef GE_SHADOW_FILTER_GLSL
#define GE_SHADOW_FILTER_GLSL

#include "compat_profile.glsl"
#include "shadow_receiver_plane.glsl"

// A separable [1,4,6,4,1] kernel convolved with bilinear reconstruction
// touches six texels on each axis. Pair their weights (a,b): one hardware
// sample at b/(a+b), weighted by a+b, reproduces that pair exactly.
// This direct derivation reduces 25 weighted PCF samples to nine.
void GE_ShadowPcfAxis(float phase, out vec3 offsets, out vec3 weights)
{
    vec3 left = vec3(1.0 - phase, 6.0 - 2.0 * phase, 1.0 + 3.0 * phase);
    vec3 right = vec3(4.0 - 3.0 * phase, 4.0 + 2.0 * phase, phase);
    weights = left + right;
    offsets = vec3(-2.0, 0.0, 2.0) + right / weights;
}

float GE_ShadowPcf5x5(sampler2DArrayShadow shadowMap, vec2 uv, float layer, float depth, vec2 receiverGradient)
{
    vec2 size = vec2(textureSize(shadowMap, 0).xy);
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
                GE_SHADOW_TAP(shadowMap, vec4(sampleUV, layer, depth +
                    dot(sampleUV - uv, receiverGradient) + dot(abs(receiverGradient), 1.0 / size)));
        }
    return visibility / 256.0;
}

float GE_ShadowPcf5x5(sampler2DArrayShadow shadowMap, vec2 uv, float layer, float depth)
{
    return GE_ShadowPcf5x5(shadowMap, uv, layer, depth, vec2(0.0));
}

// The authored world offset remains an upper bound. Limit normal displacement
// to the cascade's reconstruction footprint so fine near shadows stay attached
// to small receivers. Blend texelWorld between cascades BEFORE calling this
// function so both maps sample the same biased position through the transition.
float GE_ShadowNormalBiasWorld(float authoredWorld, float texelWorld, float referenceTexelWorld,
                               int quality, float poissonSoftness, float cosTheta,
                               float maxPenumbraWorld, bool receiverPlaneBias)
{
    float cosine = clamp(cosTheta, 0.0, 1.0);
    float authoredOffset = max(authoredWorld, 0.0) * (1.0 - cosine);
    // Moment filtering has its own preblur footprint; preserve its authored bias.
    if (quality == 4)
        return authoredOffset;
    float footprintWorld = 2.0 * max(texelWorld, 0.0);
    if (quality == 0)
        footprintWorld = 3.0 * max(texelWorld, 0.0);
    else if (quality == 2)
        footprintWorld = max(footprintWorld,
            1.5 * max(poissonSoftness, 0.0) * max(referenceTexelWorld, 0.0) + texelWorld);
    else if (quality == 5 || (quality == 3 && !receiverPlaneBias))
        footprintWorld = max(footprintWorld, max(maxPenumbraWorld, 0.0) + texelWorld);
    // A plane tilted by theta needs radius*sin(theta) normal displacement to
    // cover a radius*tan(theta) depth change across the filter. Using 1-cos for
    // this geometric bound under-biases gentle slopes. Never exceed the user's
    // original angular offset, even when they deliberately author a small value.
    return min(authoredOffset, footprintWorld * sqrt(max(1.0 - cosine * cosine, 0.0)));
}

#endif // GE_SHADOW_FILTER_GLSL
