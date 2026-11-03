#ifndef GE_PLANAR_RECONSTRUCTION_GLSL
#define GE_PLANAR_RECONSTRUCTION_GLSL

float GE_PlanarCubic(float distance)
{
    float x = abs(distance);
    if (x < 1.0f) return (1.5f * x - 2.5f) * x * x + 1.0f;
    if (x < 2.0f) return ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f;
    return 0.0f;
}

// Shared with numerical tests. Samples are a clamped 4x4 neighborhood around
// floor(uv * captureSize - 0.5); fraction is the subtexel position inside it.
// Filter premultiplied RGBA together: uncovered pixels must not grow black rims.
vec4 GE_PlanarReconstruct(vec4 samples[16], vec2 fraction)
{
    vec4 lo = min(min(samples[5], samples[6]), min(samples[9], samples[10]));
    vec4 hi = max(max(samples[5], samples[6]), max(samples[9], samples[10]));
    float peak = 1e-4f;
    for (int i = 0; i < 16; ++i)
        peak = max(peak, max(samples[i].x, max(samples[i].y, samples[i].z)));
    // A color/coverage structure tensor also sees equal-luminance color edges.
    // Normalize HDR radiance without changing coverage's contribution.
    vec4 scale = vec4(1.0f / peak, 1.0f / peak, 1.0f / peak, 1.0f);
    vec4 dx = vec4(0.0f), dy = vec4(0.0f);
    for (int i = 0; i < 4; ++i)
    {
        float w = (i == 0 || i == 3) ? 1.0f : 2.0f;
        dx += (samples[i * 4 + 3] + samples[i * 4 + 2] -
               samples[i * 4 + 1] - samples[i * 4]) * w;
        dy += (samples[12 + i] + samples[8 + i] - samples[4 + i] - samples[i]) * w;
    }
    dx *= scale / 12.0f;
    dy *= scale / 12.0f;
    float xx = dot(dx, dx), yy = dot(dy, dy), xy = dot(dx, dy);
    float discriminant = sqrt(max((xx - yy) * (xx - yy) + 4.0f * xy * xy, 0.0f));
    float eigenvalue = 0.5f * (xx + yy + discriminant);
    vec2 normal = vec2(xy, eigenvalue - xx);
    if (dot(normal, normal) < 1e-10f) normal = xx >= yy ? vec2(1, 0) : vec2(0, 1);
    normal = normalize(normal);
    vec2 tangent = vec2(-normal.y, normal.x);
    // Axis-aligned edges need no diagonal smoothing. Incoherent texture uses
    // bounded cubic reconstruction instead of an arbitrary directional blur.
    float coherence = discriminant / max(xx + yy, 1e-6f);
    float edgeBlend = smoothstep(0.02f, 0.20f, eigenvalue) * coherence *
                      (2.0f * abs(normal.x * normal.y));
    vec4 cubic = vec4(0.0f), directional = vec4(0.0f);
    float weightSum = 0.0f;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            vec2 delta = vec2(float(x - 1), float(y - 1)) - fraction;
            vec4 value = samples[y * 4 + x];
            cubic += value * (GE_PlanarCubic(delta.x) * GE_PlanarCubic(delta.y));
            float across = dot(delta, normal), along = dot(delta, tangent);
            // Narrow across the edge, wider along it to soften stair steps.
            float w = exp(-0.5f * (across * across / 0.2025f + along * along / 0.9025f));
            directional += value * w;
            weightSum += w;
        }
    // Cubic negative lobes can ring around bright HDR highlights. Bound these
    // before mixing with the positive, normalized directional filter.
    return mix(clamp(cubic, lo, hi), directional / max(weightSum, 1e-6f), edgeBlend);
}
#endif
