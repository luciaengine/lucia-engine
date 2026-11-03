#ifndef GE_RAIN_RUNOFF_GLSL
#define GE_RAIN_RUNOFF_GLSL
#include "Includes/rain_impacts.glsl"

// Derive every bead anchor from the surface's world position, before orienting
// its normal toward a camera. Camera motion, UV layout and render-origin changes
// must not reseed the water. Parallel faces share the same world-aligned field.
vec2 GE_RainSurfaceCoordinates(vec3 positionWS, vec3 normalWS)
{
    return vec2(abs(normalWS.z) > abs(normalWS.x) ? positionWS.x : positionWS.z,
                positionWS.y);
}

struct GE_RunoffMotion
{
    float cycle;
    float age;
    float travel;
    float duration;
    float split;
    vec4 intervals;
};

GE_RunoffMotion GE_SampleRunoff(vec2 id, float seconds, float rate)
{
    GE_RunoffMotion m;
    m.duration = 1.0 / max(rate, 0.001);
    float clock = seconds * rate + GE_RainHash(id + 53.0) * 19.0;
    m.cycle = floor(clock);
    m.age = fract(clock);
    float seed = GE_RainHash(id + m.cycle * 7.3);
    float other = GE_RainHash(id + m.cycle * 11.7 + 91.0);
    // Two smooth slides separated by a true stationary interval. Each bead
    // changes its hold times and travel split on its next release cycle.
    m.intervals = vec4(mix(0.05, 0.18, seed), mix(0.30, 0.43, other),
                       mix(0.55, 0.76, seed), mix(0.86, 0.96, other));
    m.split = mix(0.25, 0.70, GE_RainHash(id + m.cycle + 37.0));
    m.travel = m.split * smoothstep(m.intervals.x, m.intervals.y, m.age) +
               (1.0 - m.split) * smoothstep(m.intervals.z, m.intervals.w, m.age);
    return m;
}

float GE_InverseSmoothstep(float value)
{
    return 0.5 - sin(asin(clamp(1.0 - 2.0 * value, -1.0, 1.0)) / 3.0);
}

float GE_RunoffTimeSincePass(GE_RunoffMotion m, float travelAtPixel)
{
    // Invert the same motion, so an old trail keeps drying during a stop.
    // Dividing head distance by current speed would freeze or erase the trail.
    float passedAt;
    if (travelAtPixel <= m.split)
        passedAt = mix(m.intervals.x, m.intervals.y,
                       GE_InverseSmoothstep(clamp(travelAtPixel / m.split, 0.0, 1.0)));
    else
        passedAt = mix(m.intervals.z, m.intervals.w,
                       GE_InverseSmoothstep(clamp((travelAtPixel - m.split) / (1.0 - m.split), 0.0, 1.0)));
    return max((m.age - passedAt) * m.duration, 0.0);
}
#endif
