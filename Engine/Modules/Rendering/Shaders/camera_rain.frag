#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(push_constant) uniform CameraRainPC
{
    float shaderAnimationTime;
    float lensRainIntensity;
    float lensRefraction;
} pc;

float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Evaluate neighboring cells so moving drops never clip against a cell edge.
vec2 Drops(vec2 uv, float scale, float seed)
{
    vec2 p = uv * scale;
    vec2 cell = floor(p);
    vec2 bend = vec2(0.0);
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        vec2 id = cell + vec2(x, y);
        float h = Hash(id + seed);
        if (h > clamp(pc.lensRainIntensity, 0.0, 1.0) * 0.58)
            continue;
        float clock = pc.shaderAnimationTime * (0.035 + 0.025 * h) + Hash(id + seed + 9.0);
        float cycle = floor(clock);
        float age = fract(clock);
        vec2 anchor = vec2(Hash(id + cycle + seed + 2.0), Hash(id + cycle + seed + 5.0));
        anchor = 0.2 + anchor * 0.6;
        float slide = smoothstep(0.48, 0.82, age) * (h < 0.25 ? 0.8 : 0.0);
        vec2 q = p - id - anchor - vec2(0.025 * sin(age * 12.0 + h * 20.0), slide);
        float life = smoothstep(0.0, 0.055, age) * (1.0 - smoothstep(0.78, 1.0, age));
        float radius = mix(0.075, 0.18, Hash(id + seed + 17.0)) * sqrt(life);
        q.y *= mix(1.0, 0.62, smoothstep(0.45, 0.65, age) * (h < 0.25 ? 1.0 : 0.0));
        float r = length(q) / max(radius, 0.001);
        float shape = 1.0 - smoothstep(0.65, 1.0, r);
        bend += q / max(radius, 0.001) * shape * life;
    }
    return bend;
}

void main()
{
    vec2 size = vec2(textureSize(uSceneColor, 0));
    vec2 uv = vUV * vec2(size.x / size.y, 1.0);
    vec2 bend = Drops(uv, 9.0, 3.0) + 0.45 * Drops(uv, 19.0, 71.0);
    // Clear water only bends the scene; it does not paint dark or white droplets.
    vec2 sampleUv = clamp(vUV + bend * pc.lensRefraction / size, 0.5 / size, 1.0 - 0.5 / size);
    oColor = texture(uSceneColor, sampleUv);
}
