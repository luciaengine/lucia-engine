#ifndef GE_WATER_WEATHER_GLSL
#define GE_WATER_WEATHER_GLSL
#include "weather_wind_resolve.glsl"

float GE_WaterDropScale(float globalSize, float offset)
{
    return clamp(globalSize + offset, 0.25, 2.0);
}

// Sample at the surface/drop world position; the camera never selects the wind.
// xyz = wind velocity with gusts, w = local rain density multiplier.
vec4 GE_WaterWeather(vec3 world, float seconds)
{
    GE_ResolvedWind wind = GE_ResolveWeatherWind(world, GE_DefaultWind());
    float phase = seconds * wind.frequency * 6.2831853 + dot(world, vec3(0.73, 0.19, 0.51)) / wind.gustScale;
    float gust = sin(phase) * 0.65 + sin(phase * 1.73 + 2.1) * 0.35;
    vec3 velocity = wind.velocity * max(0.0, 1.0 + min(wind.turbulence, 2.0) * gust * 0.45);
    return vec4(velocity * min(1.0, 25.0 / max(length(velocity), 0.001)), wind.rainAmount);
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
