#ifndef GE_RAIN_STREAK_PARAMS_GLSL
#define GE_RAIN_STREAK_PARAMS_GLSL
layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#include "Includes/view_params_fields.glsl"
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;
// Shared by both stages, populated by the world light upload.
layout(set = 0, binding = 3, std140) uniform RainLightsBlock
{
    mat4 lightVP;
    vec4 direction;
    vec4 color;
    vec4 ambient;
    vec4 time;
    vec4 secondaryCount;
    vec4 secondaryDirs[3];
    vec4 secondaryColors[3];
#include "weather_wind_fields.glsl"
} RainLights;
layout(push_constant) uniform RainStreakPC
{
    float shaderAnimationTime;
    float frameDeltaTime;
    float rainIntensity;
    float rainSpeed;
    float fallHeight;
    float groundHeight;
    float dropDiameter;
    float shutterSeconds;
    float opacity;
    float fadeDistance;
    int cellMinX;
    int cellMinZ;
    int cellCountX;
    int cellCountZ;
    float waterDropSize;
    float fallingRainSizeOffset;
} pc;
#endif
