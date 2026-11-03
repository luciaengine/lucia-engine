// PBR objects with downward wet trails and moving beads on their vertical faces.
// @property float waterDropSizeOffset "Water Drop Size Offset" default=0 range=-1.75,1.75
// @property color surfaceColor "Surface Color" default=0.3,0.3,0.3
// @property float surfaceRoughness "Surface Roughness" default=0.7 range=0.02,1
// @property float surfaceMetallic "Surface Metallic" default=0 range=0,1
// @property float rainIntensity "Rain Intensity" default=0.7 range=0,1
// @property float rainSpeed "Rain Speed" default=1 range=0.1,3
// @property float dryingTime "Runoff Drying Time (s)" default=2.5 range=0.2,12
// @property float runoffDensity "Runoff Density" default=0.35 range=0,1
// @property float waterRippleStrength "Water Ripple Strength" default=0.12 range=0,0.5
// @property float waterRippleSize "Water Ripple Size (m)" default=0.1 range=0.025,1
// @property float runoffStrength "Runoff Strength" default=0.65 range=0,1
#include "Includes/water_weather.glsl"
#include "Includes/rain_runoff.glsl"
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    float dropSize = GE_WaterDropScale(Light.uWeatherWindCount.y, Props.waterDropSizeOffset);
    float waterTime = Light.uTimeParams.x * Light.uWeatherWindCount.z;
    vec4 weather = GE_WaterWeather(sIn.positionWS, waterTime);
    o.baseColor = Props.surfaceColor;
    o.roughness = Props.surfaceRoughness;
    o.metallic = Props.surfaceMetallic;
    o.normalWS = normalize(sIn.normalWS);
    vec3 n = normalize(sIn.normalWS), p = sIn.positionWS;
    vec2 worldSurface = GE_RainSurfaceCoordinates(p, n);
    float side = 1.0 - smoothstep(0.1, 0.65, abs(n.y));
    float horizontal = worldSurface.x;
    float pixelWidth = max(fwidth(horizontal), 0.001);
    float wet = 0.0, film = 0.0;
    vec2 waterSlope = vec2(0.0);
    // Unequal lane scales, sparse activation and independent lengths/speeds
    // break up the regular stripes of a single repeated vertical pattern.
    for (int laneNeighbor = -1; laneNeighbor <= 1; ++laneNeighbor)
    for (int layer = 0; layer < 2; ++layer)
    {
        float spacing = layer == 0 ? 0.37 : 0.61;
        float lane = floor(horizontal / spacing) + float(laneNeighbor);
        vec2 id = vec2(lane, float(layer) * 31.0);
        float seed = GE_RainHash(id + 13.0);
        float center = (lane + 0.12 + seed * 0.76) * spacing;
        float width = max(mix(0.0025, 0.009, GE_RainHash(id + 27.0)) * dropSize, pixelWidth);
        float across = (horizontal - center) / width;
        float column = exp(-across * across);
        if (column < 0.001)
            continue;
        float segment = mix(0.65, 1.75, GE_RainHash(id + 48.0));
        float rate = mix(0.12, 0.65, GE_RainHash(id + 72.0));
        for (int neighbor = -1; neighbor <= 1; ++neighbor)
        {
            float cell = floor(p.y / segment) + float(neighbor);
            vec2 beadId = id + vec2(0.0, cell * 17.0);
            GE_RunoffMotion motion = GE_SampleRunoff(beadId, Light.uTimeParams.x, rate * Props.rainSpeed);
            float enabled = GE_RainActive(beadId, motion.cycle, Props.runoffDensity * weather.w) *
                            smoothstep(0.0, 0.04, motion.age) * (1.0 - smoothstep(0.96, 1.0, motion.age));
            float aboveHead = p.y - (cell + 1.0 - motion.travel) * segment;
            float length = mix(0.12, 0.65, GE_RainHash(beadId + motion.cycle + 5.0));
            float trail = smoothstep(-0.015, 0.025, aboveHead) *
                          (1.0 - smoothstep(length * 0.4, length, aboveHead));
            float bead = exp(-pow(aboveHead / (0.035 * dropSize), 2.0));
            if (trail == 0.0 && bead < 0.001)
                continue;
            // Points above the moving head were wetted earlier. Their trace
            // narrows and fades in place, restoring the dry color and roughness.
            float sincePass = GE_RunoffTimeSincePass(motion, cell + 1.0 - p.y / segment);
            float remaining = 1.0 - smoothstep(0.0, Props.dryingTime, sincePass);
            float dryingColumn = exp(-pow(across / max(0.15, remaining), 2.0));
            float water = (dryingColumn * trail * remaining * 0.35 + column * bead * 0.65) * enabled;
            wet += water;
            // A thin, smooth film covers the trail even when it barely darkens
            // the substrate. The bead has a rounded head and a raised meniscus.
            film += (dryingColumn * trail * remaining + column * bead) * enabled;
            waterSlope += vec2(across * (dryingColumn * trail * remaining * 0.6 + column * bead),
                               (aboveHead / (0.035 * dropSize)) * column * bead) * enabled;
        }
    }
    float moisture = side * clamp(Props.rainIntensity * weather.w, 0.0, 1.0) * Props.runoffStrength;
    wet = min(wet, 1.0) * moisture;
    film = clamp(film * moisture, 0.0, 1.0);
    o.baseColor *= 1.0 - wet * 0.3;
    vec3 tangent = abs(n.z) > abs(n.x) ? vec3(1, 0, 0) : vec3(0, 0, 1);
    vec2 windOnWall = vec2(dot(weather.xyz, tangent), weather.y);
    vec2 ripple = GE_WaterRippleNormal(worldSurface, windOnWall, waterTime,
                                      Props.waterRippleSize * dropSize) * Props.waterRippleStrength * film;
    vec2 slope = waterSlope * moisture * 3.0 + ripple;
    vec3 waterNormal = normalize(n + tangent * slope.x + vec3(0, slope.y, 0));
    // Water reflects the sky and local lights as a separate dielectric layer;
    // the rough wall underneath retains its own normal and material response.
    o.clearCoat = film;
    o.clearCoatIor = 1.333;
    o.clearCoatRoughness = 0.06;
    o.coatNormalWS = waterNormal;
    o.coatDarkening = 0.15;
#ifdef GE_CLEARCOAT_ENABLED
    o.roughness = mix(o.roughness, o.roughness * 0.75, wet);
#ifndef GE_COAT_NORMAL_ENABLED
    o.normalWS = waterNormal;
#endif
#else
    // Materials without the optional coat lobe still get a glossy wet region.
    o.roughness = mix(o.roughness, 0.055, smoothstep(0.0, 0.55, film));
    o.normalWS = waterNormal;
#endif
    return o;
}
