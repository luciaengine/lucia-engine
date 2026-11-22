#include "Includes/weather_wind_resolve.glsl"

struct GE_FoliageWind
{
    vec2 direction;
    float strength;
    float phase;
    float frequency;
};

GE_FoliageWind GE_SampleFoliageWind(InstanceData inst)
{
    if (Light.uWeatherWindCount.x < 1.0)
        return GE_FoliageWind(vec2(1.0, 0.0), 0.0, 0.0, 0.0);
    vec3 root = vec3(inst.sector) * GE_SECTOR_SIZE + inst.modelMatrix[3].xyz;
    GE_ResolvedWind wind = GE_ResolveWeatherWind(root, GE_DefaultWind());
    float speed = length(wind.velocity.xz);
    if (speed < 0.0001)
        return GE_FoliageWind(vec2(1.0, 0.0), 0.0, 0.0, 0.0);

    vec3 horizontalWind = vec3(wind.velocity.x, 0.0, wind.velocity.z);
    vec2 direction = vec2(dot(horizontalWind, normalize(inst.modelMatrix[0].xyz)),
                          dot(horizontalWind, normalize(inst.modelMatrix[2].xyz)));
    direction /= max(length(direction), 0.0001);
    float phase = dot(root.xz, vec2(0.6, 0.8)) / wind.gustScale;
    return GE_FoliageWind(direction, min(speed * 0.1, 1.0), phase, wind.frequency);
}

float GE_FoliageWindHeight(float localY, vec2 baseAndHeight)
{
    return clamp((localY - baseAndHeight.x) / max(baseAndHeight.y, 0.001), 0.0, 1.0);
}

vec3 GE_FoliageSway(GE_FoliageWind wind, float time, float strength, float swayMeters)
{
    float gust = sin(6.2831853 * (time * wind.frequency + wind.phase));
    vec2 offset = wind.direction * (gust * max(swayMeters, 0.0) * strength);
    return vec3(offset.x, 0.0, offset.y);
}

vec3 GE_ApplyFoliageSway(vec3 position, InstanceData inst, vec2 baseAndHeight, float swayMeters)
{
    if (swayMeters <= 0.0)
        return position;
    GE_FoliageWind wind = GE_SampleFoliageWind(inst);
    float strength = GE_FoliageWindHeight(position.y, baseAndHeight) * wind.strength;
    return position + GE_FoliageSway(wind, inst.deformationTimeSeconds, strength, swayMeters);
}
