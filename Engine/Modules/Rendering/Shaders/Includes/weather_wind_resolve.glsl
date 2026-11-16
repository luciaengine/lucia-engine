// Wind at a world position from the weather wind volumes in the light block.
// GPU mirror of WindVolumeResolver::ResolveAt: the same shapes, smooth boundary fade and
// ordered blend over a base wind. The block holds the 32 highest-priority volumes.
// The includer declares the block that holds weather_wind_fields.glsl and names it with
// GE_WEATHER_LIGHT (default Light).

#ifndef GE_WEATHER_WIND_RESOLVE_GLSL
#define GE_WEATHER_WIND_RESOLVE_GLSL
#ifndef GE_WEATHER_LIGHT
#define GE_WEATHER_LIGHT Light
#endif

struct GE_ResolvedWind
{
    vec3 velocity;   // m/s, world space
    float turbulence;
    float frequency; // gust frequency, Hz
    float gustScale; // spatial gust scale, m
    float rainAmount;
};

GE_ResolvedWind GE_DefaultWind()
{
    GE_ResolvedWind wind;
    wind.velocity = vec3(0.0);
    wind.turbulence = 0.0;
    wind.frequency = 0.5;
    wind.gustScale = 70.0;
    wind.rainAmount = 1.0;
    return wind;
}

// `world` is in the frame of sector (0,0,0), the frame the volume centers use.
GE_ResolvedWind GE_ResolveWeatherWind(vec3 world, GE_ResolvedWind wind)
{
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
            wind.velocity = mix(wind.velocity, target.xyz, influence);
            wind.turbulence = mix(wind.turbulence, gust.x, influence);
        }
        else if (dot(target.xyz, target.xyz) == 0.0)
        {
            wind.velocity *= 1.0 - influence;
            wind.turbulence *= 1.0 - influence;
        }
        else
        {
            wind.velocity += target.xyz * influence;
            wind.turbulence += gust.x * influence;
        }
        // Additive amount is a delta from the neutral multiplier (1).
        wind.rainAmount = mode.y > 0.5 ? mix(wind.rainAmount, mode.z, influence) :
                                        wind.rainAmount + (mode.z - 1.0) * influence;
        wind.frequency = mix(wind.frequency, gust.y, influence);
        wind.gustScale = mix(wind.gustScale, gust.z, influence);
    }
    wind.rainAmount = clamp(wind.rainAmount, 0.0, 2.0);
    wind.turbulence = max(wind.turbulence, 0.0);
    wind.frequency = max(wind.frequency, 0.0);
    wind.gustScale = max(wind.gustScale, 0.001);
    return wind;
}

#endif
