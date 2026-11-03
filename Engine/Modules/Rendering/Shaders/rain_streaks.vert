#version 450
#include "Includes/rain_streak_params.glsl"
#include "Includes/rain_impacts.glsl"
#define GE_WEATHER_LIGHT RainLights
#define GE_WATER_WEATHER_VERTEX
#include "Includes/water_weather.glsl"

layout(location = 0) noperspective out vec2 vCoord;
layout(location = 1) flat out vec4 vShape;
layout(location = 2) flat out vec3 vPositionWS;
layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

vec4 Project(vec3 world)
{
    vec4 clip = ViewParams.ge_viewProj * vec4(world, 1.0);
    // Composited after TAA: use the resolved, unjittered camera.
    clip.xy -= ViewParams.ge_taaJitter.xy * clip.w;
    return clip;
}

void main()
{
    vCoord = vec2(0.0);
    vShape = vec4(0.0);
    vPositionWS = vec3(0.0);
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    int columns = max(pc.cellCountX, 1);
    if (gl_InstanceIndex >= columns * max(pc.cellCountZ, 0))
        return;
    vec2 id = vec2(pc.cellMinX + gl_InstanceIndex % columns,
                   pc.cellMinZ + gl_InstanceIndex / columns);
    GE_RainImpact impact = GE_SampleRainImpact(id, pc.shaderAnimationTime, pc.rainSpeed);
    vec4 weather = GE_WaterWeather(vec3(impact.center.x, pc.groundHeight, impact.center.y), pc.shaderAnimationTime);
    if (GE_RainActive(id, impact.cycle + 1.0, pc.rainIntensity * weather.w) == 0.0)
        return;
    float height = max(pc.fallHeight, 0.01);
    float rate = max((0.65 + GE_RainHash(id + 73.1) * 0.45) * pc.rainSpeed, 0.001);
    float exposure = clamp(pc.shutterSeconds, 0.0, 0.05);
    vec3 tip = vec3(impact.center.x, pc.groundHeight + height * (1.0 - impact.phase), impact.center.y);
    // Integrate backwards from the live tip; its landing stays exactly in phase
    // with the ground ripple and ballistic burst, including the cycle rollover.
    vec3 tail = tip + vec3(0.0, min(height * rate * exposure, height * impact.phase), 0.0);
    vec3 wind = GE_WaterWeather(tip, pc.shaderAnimationTime).xyz;
    // Integrate backwards from the fixed impact anchor; wind bends the path
    // without moving the landing event away from its ripple/splash.
    float remaining = (1.0 - impact.phase) / rate;
    tip.xz -= wind.xz * remaining * 0.18;
    tail.xz -= wind.xz * (remaining + (tail.y - tip.y) / (height * rate)) * 0.18;
    vec4 tipClip = Project(tip);
    vec4 tailClip = Project(tail);
    if (min(tipClip.w, tailClip.w) <= max(ViewParams.ge_nearFar.x, 0.025))
        return;
    vec2 tipNdc = tipClip.xy / tipClip.w;
    vec2 tailNdc = tailClip.xy / tailClip.w;
    vec4 previous = ViewParams.ge_prevViewProj * vec4(tail, 1.0);
    if (previous.w > 0.025 && pc.frameDeltaTime > 0.0001 && pc.frameDeltaTime < 0.2)
    {
        vec2 cameraMotion = previous.xy / previous.w - tailNdc;
        // Camera cuts must not stretch rain across the screen.
        if (length(cameraMotion) < 0.2)
            tailNdc += cameraMotion * clamp(exposure / pc.frameDeltaTime, 0.0, 2.0);
    }
    // The resolved target may be larger than ViewParams' internal TAA extent.
    vec2 screen = max(vec2(textureSize(uSceneColor, 0)), vec2(1.0));
    vec2 deltaPx = (tailNdc - tipNdc) * screen * 0.5;
    float streakPixels = length(deltaPx);
    vec2 axis = streakPixels > 0.001 ? deltaPx / streakPixels : vec2(0.0, 1.0);
    streakPixels = min(streakPixels, 96.0);
    vec2 side = vec2(-axis.y, axis.x);
    float diameter = clamp(pc.dropDiameter, 0.0005, 0.006) *
                     GE_WaterDropScale(pc.waterDropSize, pc.fallingRainSizeOffset) * mix(0.7, 1.3, GE_RainHash(id + impact.cycle));
    float radius = diameter * abs(ViewParams.ge_proj[1][1]) * screen.y * 0.25 / tipClip.w;
    float rasterRadius = max(radius, 0.65) + 0.8;
    const vec2 corners[6] = vec2[6](vec2(-1, 0), vec2(1, 0), vec2(1, 1),
                                  vec2(-1, 0), vec2(1, 1), vec2(-1, 1));
    vec2 corner = corners[gl_VertexIndex % 6];
    float along = mix(-rasterRadius, streakPixels + rasterRadius, corner.y);
    vec2 ndc = tipNdc + (side * corner.x * rasterRadius + axis * along) * 2.0 / screen;
    float distanceToCamera = length(tip - ViewParams.ge_cameraPosWS.xyz);
    float fade = smoothstep(0.12, 0.65, distanceToCamera) *
                 (1.0 - smoothstep(pc.fadeDistance * 0.65, max(pc.fadeDistance, 0.1), distanceToCamera));
    float birth = smoothstep(0.0, max(exposure * rate, 0.001), impact.phase);
    vCoord = vec2(corner.x * rasterRadius, along);
    vShape = vec4(radius, streakPixels, clamp(pc.opacity, 0.0, 1.0) * fade * birth, tipClip.w);
    vPositionWS = tip;
    gl_Position = vec4(ndc * tipClip.w, tipClip.z, tipClip.w);
}
