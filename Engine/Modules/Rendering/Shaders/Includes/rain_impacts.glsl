#ifndef GE_RAIN_IMPACTS_GLSL
#define GE_RAIN_IMPACTS_GLSL
const float GE_RAIN_CELL_SIZE = 0.60;
float GE_RainHash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec2 GE_RainCenter(vec2 id)
{
    uint h = uint(int(id.x)) * 1664525u ^ uint(int(id.y)) * 1013904223u;
    h ^= h >> 16u;
    vec2 jitter = vec2(h & 65535u, (h >> 16u) & 65535u) / 65535.0;
    return (id + 0.15 + 0.70 * jitter) * GE_RAIN_CELL_SIZE;
}
struct GE_RainImpact
{
    vec2 center;
    float phase;
    float cycle;
    float secondsSinceImpact;
};
GE_RainImpact GE_SampleRainImpact(vec2 id, float seconds, float speed)
{
    float seed = GE_RainHash(id + 73.1);
    float rate = max((0.65 + seed * 0.45) * speed, 0.001);
    float clock = seconds * rate + seed * 19.0;
    GE_RainImpact impact;
    impact.center = GE_RainCenter(id);
    impact.phase = fract(clock);
    impact.cycle = floor(clock);
    impact.secondsSinceImpact = impact.phase / rate;
    return impact;
}
float GE_RainActive(vec2 id, float cycle, float intensity)
{
    return intensity > 0.0 ? step(GE_RainHash(id + cycle + 41.0), intensity) : 0.0;
}
#endif
