#version 450
#include "Includes/rain_streak_params.glsl"
layout(location = 0) noperspective in vec2 vCoord;
layout(location = 1) flat in vec4 vShape;
layout(location = 2) flat in vec3 vPositionWS;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 5) uniform samplerCube ge_irradianceCube;
// Prefix of the world-material EnvDataUBO (ibl.glsl).
layout(set = 0, binding = 6, std140) uniform EnvironmentBlock
{
    float iblIntensity;
} Env;
struct RainLocalLight
{
#include "Includes/light_packed_fields.glsl"
};
layout(set = 0, binding = 4, std430) readonly buffer LightBuffer
{
    uvec4 lightHeader;
    RainLocalLight localLights[];
};

float Phase(vec3 direction, vec3 toEye)
{
    // Broad side response and a brighter lobe looking towards the light.
    float cosTheta = dot(direction * inversesqrt(max(dot(direction, direction), 1e-8)), toEye);
    return 0.12 + 0.88 * pow(max(cosTheta, 0.0), 6.0);
}

void main()
{
    float along = vCoord.y - clamp(vCoord.y, 0.0, vShape.y);
    float r = length(vec2(vCoord.x, along));
    float filterRadius = max(vShape.x, 0.65);
    float coverage = exp2(-2.5 * r * r / (filterRadius * filterRadius));
    // Widen subpixel drops for filtering, conserving their integrated coverage.
    float alpha = coverage * min(vShape.x / filterRadius, 1.0) * vShape.z;
    if (alpha < 0.002)
        discard;
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uSceneColor, 0));
    vec2 depthUv = uv + ViewParams.ge_taaJitter.xy * vec2(0.5, -0.5);
    float depth = texture(uDepth, depthUv).r;
    vec2 ndc = vec2(depthUv.x * 2.0 - 1.0, 1.0 - depthUv.y * 2.0);
    vec4 sceneView = ViewParams.ge_invProj * vec4(ndc, depth, 1.0);
    float sceneDistance = abs(sceneView.z / max(abs(sceneView.w), 1e-7));
    // Reverse-Z far (zero) is sky. Feather intersections with opaque geometry.
    if (depth > 0.000001)
        alpha *= smoothstep(0.0, 0.12, sceneDistance - vShape.w);
    if (alpha < 0.002)
        discard;
    vec3 toEye = normalize(ViewParams.ge_cameraPosWS.xyz - vPositionWS);
    vec3 radiance = max(RainLights.ambient.rgb * RainLights.ambient.w, vec3(0.0)) * 0.25;
    // A droplet gathers light over a broad solid angle. The convolved sky cube
    // supplies that fill in scene HDR units, before the camera's exposure.
    vec3 skyDirection = normalize(vec3(toEye.x, max(toEye.y, 0.35), toEye.z));
    radiance += max(textureLod(ge_irradianceCube, skyDirection, 0.0).rgb, vec3(0.0)) *
                max(Env.iblIntensity, 0.0) * 0.65;
    if (RainLights.direction.w > 0.0)
        radiance += max(RainLights.color.rgb, vec3(0.0)) * RainLights.direction.w *
                    Phase(RainLights.direction.xyz, toEye) * 0.05;
    for (int i = 0; i < min(int(RainLights.secondaryCount.x), 3); ++i)
        radiance += max(RainLights.secondaryColors[i].rgb, vec3(0.0)) *
                    Phase(RainLights.secondaryDirs[i].xyz, toEye) * 0.05;
    // Small weather fields need only a bounded punctual-light walk. This makes
    // droplets passing the streetlamps pick up their actual colour and range.
    for (uint i = 0u; i < min(lightHeader.x, 32u); ++i)
    {
        RainLocalLight light = localLights[i];
        if (light.meta.w == 0u || (light.meta.x != 1u && light.meta.x != 2u))
            continue;
        vec3 fromLight = vPositionWS - light.posRange.xyz;
        float distanceSquared = dot(fromLight, fromLight);
        float rangeSquared = max(light.posRange.w * light.posRange.w, 0.001);
        float attenuation = pow(max(1.0 - distanceSquared / rangeSquared, 0.0), 2.0) /
                            max(distanceSquared, 0.25);
        if (light.meta.x == 2u)
            attenuation *= smoothstep(light.spotParams.x, light.areaParams.w,
                                       dot(normalize(fromLight), light.dirIntensity.xyz));
        radiance += max(light.colorAreaWidth.rgb * light.dirIntensity.w, vec3(0.0)) *
                    attenuation * Phase(fromLight, toEye) * 0.05;
    }
    // Most streaks only scatter light. Close drops retain a restrained lens cue.
    float lens = 1.0 - smoothstep(1.0, 3.0, vShape.w);
    if (lens > 0.0)
    {
        vec2 bend = vec2(clamp(vCoord.x / filterRadius, -1.0, 1.0), 0.0) /
                    vec2(textureSize(uSceneColor, 0));
        vec3 refracted = texture(uSceneColor, clamp(uv + bend, vec2(0.0), vec2(1.0))).rgb;
        vec3 background = texture(uSceneColor, uv).rgb;
        radiance += max(refracted - background, vec3(0.0)) * lens;
    }
    // Additive, premultiplied coverage: dark droplets become transparent and
    // never paint a grey gradient over the pavement. Preserve destination alpha.
    oColor = vec4(max(radiance, vec3(0.0)) * alpha, 0.0);
}
