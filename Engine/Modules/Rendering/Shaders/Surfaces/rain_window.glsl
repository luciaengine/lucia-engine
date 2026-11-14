// Thin transparent window panes with downward-moving water beads and trails.
// @property float waterDropSizeOffset "Water Drop Size Offset" default=0 range=-1.75,1.75
// @property color glassTint "Glass Tint" default=0.96,0.98,1
// @property float dryingTime "Drip Drying Time (s)" default=3 range=0.2,12
// @property float frostEdgeWidth "Frosted Border Width" default=0.08 range=0,0.3
// @property float staticDropStrength "Static Drop Strength" default=0.55 range=0,0.8
// @property float rainIntensity "Rain Intensity" default=0.8 range=0,1
// @property float rainSpeed "Rain Speed" default=1 range=0.1,3
// @property float waterRippleStrength "Water Ripple Strength" default=0.12 range=0,0.5
// @property float waterRippleSize "Water Ripple Size (m)" default=0.1 range=0.025,1
// @property float dropNormalStrength "Drop Normal Strength" default=0.35 range=0,0.8
#define GE_TWO_SIDED_KEEP_NORMAL
#include "Includes/water_weather.glsl"
#include "Includes/rain_runoff.glsl"
// Two independent, jittered bead sizes cover the full pane, including its center.
// Return the lens slope and wet footprint; stationary droplets do not slide.
vec3 WindowBeads(vec2 worldSurface, vec2 cellsPerMeter, float salt, float intensity, float dropSize)
{
    vec2 p = worldSurface * cellsPerMeter;
    float footprint = max(length(dFdx(p)), length(dFdy(p)));
    vec3 result = vec3(0.0);
    // Enlarging a bead must not clip it at its original cell boundary.
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        vec2 id = floor(p) + vec2(x, y);
        vec2 center = 0.24 + 0.52 * vec2(GE_RainHash(id + salt), GE_RainHash(id + salt + 17.0));
        float enabled = GE_RainActive(id + salt, 0.0, intensity);
        float radius = mix(0.13, 0.23, GE_RainHash(id + salt + 31.0)) * dropSize;
        float filteredRadius = max(radius, footprint * 0.6);
        float visibility = pow(radius / filteredRadius, 2.0) *
                           (1.0 - smoothstep(0.55, 1.5, footprint));
        vec2 shape = (p - id - center) / (filteredRadius * vec2(0.85, 1.15));
        float r2 = dot(shape, shape);
        if (r2 >= 1.45 || enabled == 0.0) continue;
        float bead = exp(-r2 * 1.8) * (1.0 - smoothstep(1.0, 1.45, r2)) * enabled * visibility;
        result += vec3(shape * bead, bead);
    }
    return result;
}
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    float dropSize = GE_WaterDropScale(Light.uWeatherWindCount.y, Props.waterDropSizeOffset);
    float waterTime = Light.uTimeParams.x * Light.uWeatherWindCount.z;
    vec4 weather = GE_WaterWeather(sIn.positionWS, waterTime);
    vec3 n = normalize(sIn.normalWS);
    vec2 worldSurface = GE_RainSurfaceCoordinates(sIn.positionWS, n);
    float horizontal = worldSurface.x;
    float lane = floor(horizontal * 13.0);
    float vertical = worldSurface.y * 5.0;
    float bead = 0.0, trail = 0.0;
    vec2 slope = vec2(0.0);
    // Neighbor cells preserve tails that cross a segment boundary. Every head
    // owns a separate clock, including vertically adjacent beads in one lane.
    for (int laneNeighbor = -1; laneNeighbor <= 1; ++laneNeighbor)
    for (int neighbor = -1; neighbor <= 1; ++neighbor)
    {
        float cell = floor(vertical) + float(neighbor);
        vec2 id = vec2(lane + float(laneNeighbor), cell);
        float seed = GE_RainHash(id + 17.0);
        GE_RunoffMotion motion = GE_SampleRunoff(id, Light.uTimeParams.x,
                                                Props.rainSpeed * mix(0.18, 0.36, seed));
        vec2 q = vec2(horizontal * 13.0 - id.x - (0.15 + seed * 0.7),
                       vertical - (cell + 1.0 - motion.travel));
        if (abs(q.x) > 0.4 * dropSize || q.y < -0.45 * dropSize || q.y > max(0.5, 0.4 * dropSize))
            continue;
        float enabled = GE_RainActive(id, motion.cycle, Props.rainIntensity * weather.w) *
                        smoothstep(0.0, 0.04, motion.age) * (1.0 - smoothstep(0.96, 1.0, motion.age));
        vec2 shape = q * vec2(8.0, 8.0) / dropSize;
        float head = exp(-dot(shape, shape)) * enabled;
        float sincePass = GE_RunoffTimeSincePass(motion, cell + 1.0 - vertical);
        float remaining = 1.0 - smoothstep(0.0, Props.dryingTime, sincePass);
        trail += exp(-pow(q.x * 12.0 / (dropSize * max(remaining, 0.15)), 2.0)) * smoothstep(0.0, 0.10, q.y) *
                 (1.0 - smoothstep(0.25, 0.48, q.y)) * enabled * remaining;
        bead += head;
        slope += shape * head * Props.dropNormalStrength;
    }
    vec3 tangent = abs(n.z) > abs(n.x) ? vec3(1, 0, 0) : vec3(0, 0, 1);
    vec3 largeBeads = WindowBeads(worldSurface, vec2(22.0), 31.0, Props.rainIntensity * weather.w, dropSize);
    vec3 fineBeads = WindowBeads(worldSurface, vec2(46.0), 101.0, Props.rainIntensity * weather.w, dropSize);
    slope += (largeBeads.xy * 1.8 + fineBeads.xy) * Props.staticDropStrength;
    float wetLens = clamp(bead + largeBeads.z + fineBeads.z, 0.0, 1.0);
    vec2 windOnPane = vec2(dot(weather.xyz, tangent), weather.y);
    slope += GE_WaterRippleNormal(worldSurface, windOnPane, waterTime,
                                 Props.waterRippleSize * dropSize) * Props.waterRippleStrength *
             clamp(wetLens + trail, 0.0, 1.0);
    float edge = min(min(sIn.uv0.x, 1.0 - sIn.uv0.x), min(sIn.uv0.y, 1.0 - sIn.uv0.y));
    float frost = Props.frostEdgeWidth > 0.0 ? 1.0 - smoothstep(0.0, Props.frostEdgeWidth, edge) : 0.0;
    o.normalWS = normalize(n + tangent * slope.x + vec3(0, slope.y, 0));
    if (dot(n, sIn.viewDirWS) < 0.0) o.normalWS = -o.normalWS;
    o.baseColor = vec3(0.01);
    o.specularIor = 1.45;
    o.roughness = mix(0.035 + trail * 0.015, 0.45, frost);
    o.transmissionWeight = 1.0;
    o.transmissionRefractionStrength = mix(0.025, 0.085, wetLens);
    o.transmissionBlur = frost * 6.0;
    // A faint directional rim keeps clear beads legible over a uniform interior.
    // Modulate transmitted light locally instead of painting opaque drop shapes.
    float lensLight = clamp(dot(slope, vec2(-0.45, 0.8)), -0.10, 0.18);
    o.transmissionColor = Props.glassTint * (1.0 + lensLight);
    o.planarReflectionMask = 0.0;
    return o;
}
