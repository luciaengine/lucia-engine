#version 450

layout(set = 0, binding = 1) uniform sampler2D uAtlas;
// The sprite's tangent-space normal atlas, laid out like uAtlas. Bound to the
// atlas itself for sprites without one; only read when vSurface.z says so.
layout(set = 0, binding = 2) uniform sampler2D uNormal;

// Mirrors SpriteLight in SpriteRenderFeature.h (std430, 48 bytes).
struct SpriteLight
{
    vec4 PositionType;     // xyz = world position (point), w = Components::LightType
    vec4 ColorRange;       // rgb = colour times intensity, w = range (point)
    vec4 DirectionFalloff; // xyz = direction the light shines (directional), w = Components::LightFalloff
};

layout(std430, set = 0, binding = 3) readonly buffer Lights
{
    SpriteLight uLights[];
};

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vTint;
layout(location = 2) in vec3 vPosition;
layout(location = 3) in vec3 vTangent;
layout(location = 4) in vec3 vBitangent;
layout(location = 5) in vec3 vNormal;
// x = lit, y = normal strength, z = has a normal map, w = light count.
layout(location = 6) flat in vec4 vSurface;

layout(location = 0) out vec4 outColor;

// Components::LightType and Components::LightFalloff values the lights carry.
const int kLightPoint = 1;
const int kLightAmbient = 3;
const int kFalloffPhysicalInverseSquare = 0;
const int kFalloffLinear = 1;
const float PI = 3.14159265359;

vec3 SurfaceNormal()
{
    vec2 tilt = vec2(0.0);
    if (vSurface.z > 0.5)
        tilt = (texture(uNormal, vUV).xy * 2.0 - 1.0) * vSurface.y;
    const vec3 n = vec3(tilt, sqrt(max(0.0001, 1.0 - dot(tilt, tilt))));
    return normalize(normalize(vTangent) * n.x + normalize(vBitangent) * n.y + normalize(vNormal) * n.z);
}

// Irradiance the frame's lights deliver to a surface facing N at vPosition.
vec3 Irradiance(vec3 N)
{
    vec3 irradiance = vec3(0.0);
    const int count = int(vSurface.w);
    for (int i = 0; i < count; ++i)
    {
        const SpriteLight light = uLights[i];
        const int type = int(light.PositionType.w);
        if (type == kLightAmbient)
        {
            irradiance += light.ColorRange.rgb;
            continue;
        }
        vec3 L = -normalize(light.DirectionFalloff.xyz);
        float attenuation = 1.0;
        if (type == kLightPoint)
        {
            const vec3 delta = light.PositionType.xyz - vPosition;
            const float distanceToLight = length(delta);
            L = delta / max(distanceToLight, 0.0001);
            const float rangeFade = max(0.0, 1.0 - distanceToLight / light.ColorRange.w);
            const int falloff = int(light.DirectionFalloff.w);
            if (falloff == kFalloffLinear)
                attenuation = rangeFade;
            else if (falloff == kFalloffPhysicalInverseSquare)
                attenuation = rangeFade * rangeFade / max(distanceToLight * distanceToLight, 0.01);
            else
                attenuation = rangeFade * rangeFade;
        }
        irradiance += light.ColorRange.rgb * max(dot(N, L), 0.0) * attenuation;
    }
    return irradiance;
}

void main()
{
    // No discard: the pass attaches no depth and writes none, so a transparent
    // texel already contributes nothing under SrcAlpha blending.
    outColor = texture(uAtlas, vUV) * vTint;
    if (vSurface.x > 0.5)
        outColor.rgb *= Irradiance(SurfaceNormal()) / PI;
}
