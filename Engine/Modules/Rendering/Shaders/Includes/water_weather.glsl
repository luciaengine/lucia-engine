#ifndef GE_WATER_WEATHER_GLSL
#define GE_WATER_WEATHER_GLSL
#ifndef GE_WEATHER_LIGHT
#define GE_WEATHER_LIGHT Light
#endif

float GE_WaterDropScale(float globalSize, float offset)
{
    return clamp(globalSize + offset, 0.25, 2.0);
}

// Same spatial shapes, smooth boundary fade and ordered blend as WindVolumeResolver.
// Sample at the surface/drop world position; the camera never selects the wind.
// xyz = wind velocity, w = local rain density multiplier.
vec4 GE_WaterWeather(vec3 world, float seconds)
{
    vec3 velocity = vec3(0.0);
    float rainAmount = 1.0;
    float turbulence = 0.0, frequency = 0.5, gustScale = 70.0;
    int count = clamp(int(GE_WEATHER_LIGHT.uWeatherWindCount.x), 0, 32);
    for (int i = 0; i < count; ++i)
    {
        int base = i * 7;
        vec4 center = GE_WEATHER_LIGHT.uWeatherWind[base];
        vec4 axisX = GE_WEATHER_LIGHT.uWeatherWind[base + 1];
        vec4 axisY = GE_WEATHER_LIGHT.uWeatherWind[base + 2];
        vec4 axisZ = GE_WEATHER_LIGHT.uWeatherWind[base + 3];
        vec4 target = GE_WEATHER_LIGHT.uWeatherWind[base + 4];
        vec4 gust = GE_WEATHER_LIGHT.uWeatherWind[base + 5];
        vec4 mode = GE_WEATHER_LIGHT.uWeatherWind[base + 6];
        vec3 delta = world - center.xyz;
        vec3 p = vec3(dot(delta, axisX.xyz), dot(delta, axisY.xyz), dot(delta, axisZ.xyz));
        vec3 extent = max(vec3(axisX.w, axisY.w, axisZ.w), vec3(0.001));
        float distanceOutside = 0.0;
        if (gust.w < 0.5)
        {
            if (mode.x < 0.5)
                distanceOutside = length(max(abs(p) - extent, vec3(0.0)));
            else if (mode.x < 1.5)
            {
                float k1 = length(p / extent), k2 = length(p / (extent * extent));
                distanceOutside = k2 > 0.0 ? max(0.0, k1 * (k1 - 1.0) / k2) : 0.0;
            }
            else
            {
                float radius = max(extent.x, extent.z);
                float core = max(0.0, extent.y - (mode.x < 2.5 ? radius : 0.0));
                float y = max(abs(p.y) - core, 0.0);
                distanceOutside = mode.x < 2.5 ? max(0.0, length(vec2(length(p.xz), y)) - radius) :
                    length(vec2(max(length(p.xz) - radius, 0.0), y));
            }
        }
        float spatial = distanceOutside <= 0.0 ? 1.0 :
                        1.0 - smoothstep(0.0, max(center.w, 0.00001), distanceOutside);
        float influence = target.w * spatial;
        if (influence <= 0.0) continue;
        if (mode.y > 0.5)
        {
            velocity = mix(velocity, target.xyz, influence);
            turbulence = mix(turbulence, gust.x, influence);
        }
        else if (dot(target.xyz, target.xyz) == 0.0)
        {
            velocity *= 1.0 - influence;
            turbulence *= 1.0 - influence;
        }
        else
        {
            velocity += target.xyz * influence;
            turbulence += gust.x * influence;
        }
        // Additive amount is a delta from the neutral multiplier (1).
        rainAmount = mode.y > 0.5 ? mix(rainAmount, mode.z, influence) :
                                   rainAmount + (mode.z - 1.0) * influence;
        frequency = mix(frequency, gust.y, influence);
        gustScale = mix(gustScale, gust.z, influence);
    }
    float phase = seconds * frequency * 6.2831853 + dot(world, vec3(0.73, 0.19, 0.51)) / max(gustScale, 0.001);
    float gust = sin(phase) * 0.65 + sin(phase * 1.73 + 2.1) * 0.35;
    velocity *= max(0.0, 1.0 + clamp(turbulence, 0.0, 2.0) * gust * 0.45);
    return vec4(velocity * min(1.0, 25.0 / max(length(velocity), 0.001)), clamp(rainAmount, 0.0, 2.0));
}

#ifndef GE_WATER_WEATHER_VERTEX
// Travelling waves perturb only the water normal. Pixel filtering prevents
// distant, narrow water films from sparkling as the phase moves.
vec2 GE_WaterRippleNormal(vec2 surface, vec2 wind, float seconds, float wavelength)
{
    vec2 p = surface / max(wavelength, 0.025);
    vec2 direction = wind / max(length(wind), 0.001);
    vec2 drift = direction * smoothstep(0.0, 0.5, length(wind));
    vec2 a = vec2(1.73, 2.41) + drift * 2.0;
    vec2 b = vec2(-2.19, 1.37) + drift;
    vec2 phase = vec2(dot(p, a), dot(p, b)) + seconds * vec2(-2.4, 1.7);
    vec2 footprint = max(abs(dFdx(phase)), abs(dFdy(phase)));
    vec2 wave = sin(phase) * (1.0 - smoothstep(vec2(1.0), vec2(3.14), footprint));
    return (normalize(a) * wave.x + normalize(b) * wave.y * 0.65) *
           (1.0 + min(length(wind) * 0.12, 1.0));
}
#endif
#endif
