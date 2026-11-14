// Opaque wet-ground surface. One flat mesh blends dry ground, damp banks and
// shallow water; the planar mask controls reflection coverage independently.
// An optional grayscale texture replaces the procedural puddle distribution.
// @texture puddleMask linear
// @property float waterDropSizeOffset "Water Drop Size Offset" default=0 range=-1.75,1.75
// @property bool useMaskTexture "Use Mask Texture" default=false
// @property color groundColor "Dry Ground Color" default=0.18,0.16,0.13
// @property float groundRoughness "Dry Roughness" default=0.88 range=0.4,1
// @property float puddleCoverage "Puddle Coverage" default=0.56 range=0,1
// @property float puddleScale "Puddle Size (m)" default=4.5 range=0.5,12
// @property float edgeSoftness "Wet Edge Width" default=0.08 range=0.01,0.25
// @property float wetDarkening "Wet Ground Darkening" default=0.72 range=0,0.95
// @property float waterRoughness "Water Roughness" default=0.045 range=0.02,0.3
// @property float rainIntensity "Rain Intensity" default=0.7 range=0,1
// @property float rainSpeed "Rain Speed" default=1 range=0.1,3
// @property float rainFieldMinX "Rain Field Min X (m)" default=-10000 range=-10000,10000
// @property float rainFieldMaxX "Rain Field Max X (m)" default=10000 range=-10000,10000
// @property float rainFieldMinZ "Rain Field Min Z (m)" default=-10000 range=-10000,10000
// @property float rainFieldMaxZ "Rain Field Max Z (m)" default=10000 range=-10000,10000
// @property float waterNormalStrength "Water Normal Strength" default=0.035 range=0,0.15
// @property float waterNormalScale "Water Normal Size (m)" default=0.6 range=0.1,3
// @property float rippleStrength "Ripple Strength" default=0.4 range=0,0.8
// @property float grainStrength "Ground Grain" default=0.3 range=0,1

#include "Includes/water_weather.glsl"
#include "Includes/rain_impacts.glsl"

float PuddleHash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float PuddleNoise(vec2 p)
{
    vec2 cell = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(PuddleHash(cell), PuddleHash(cell + vec2(1, 0)), f.x),
               mix(PuddleHash(cell + vec2(0, 1)), PuddleHash(cell + vec2(1)), f.x), f.y);
}
float PuddleField(vec2 p)
{
    // Low frequency shapes with smaller bank irregularities, no frame noise.
    return PuddleNoise(p) * 0.65 + PuddleNoise(p * 2.13 + 4.7) * 0.25 +
           PuddleNoise(p * 5.37 - 2.3) * 0.10;
}
// Each cell has a separate impact clock shared with its falling rain card.
// Neighbor cells keep rings continuous across tile boundaries.
vec4 PuddleRain(vec2 world, float seconds, float dropSize, float groundY)
{
    vec2 cell = floor(world / GE_RAIN_CELL_SIZE);
    // Different loop iterations address different hashed centers on either side
    // of a cell boundary. Derive filtering from continuous world coordinates,
    // never from per-cell distance, or quad derivatives reveal the tile grid.
    float pixelWidth = max(length(dFdx(world)), length(dFdy(world)));
    vec2 slope = vec2(0);
    float rings = 0.0, roadSplash = 0.0;
    int reach = dropSize > 1.0 ? 2 : 1;
    for (int y = -reach; y <= reach; ++y)
    for (int x = -reach; x <= reach; ++x)
    {
        vec2 id = cell + vec2(x, y);
        GE_RainImpact impact = GE_SampleRainImpact(id, seconds, Props.rainSpeed);
        vec2 center = impact.center;
        // Match the authored rain field so every shader impact has a falling
        // drop and a splash at the same position; outer dry cells stay quiet.
        if (center.x < Props.rainFieldMinX || center.x >= Props.rainFieldMaxX ||
            center.y < Props.rainFieldMinZ || center.y >= Props.rainFieldMaxZ)
            continue;
        float age = impact.phase, cycle = impact.cycle;
        float impactEnabled = GE_RainActive(id, cycle, Props.rainIntensity *
            GE_WaterWeather(vec3(center.x, groundY, center.y), seconds).w);
        vec2 delta = world - center;
        float d = length(delta);
        float radius = age * 0.48 * dropSize;
        float fade = (1.0 - age) * (1.0 - age) * smoothstep(0.0, 0.045, age) * impactEnabled;
        float width = max(0.018 * dropSize, pixelWidth * 1.25);
        float ring = exp(-pow((d - radius) / width, 2.0));
        float second = exp(-pow((d - radius + 0.065 * dropSize) / width, 2.0)) * 0.45;
        rings += (ring + second) * fade;
        float waveWidth = max(0.04 * dropSize, pixelWidth * 1.5);
        float q = (d - radius) / waveWidth;
        float q2 = (d - radius + 0.09 * dropSize) / waveWidth;
        float wave = -q * exp(-q * q) - q2 * exp(-q2 * q2) * 0.45;
        slope += delta / max(d, 0.001) * wave * fade;
        // Dry-ground marks fade continuously over 120 ms. Keep their energy
        // bounded when filtering a small impact at a grazing view angle.
        float splashLife = clamp(impact.secondsSinceImpact / 0.12, 0.0, 1.0);
        float splashRadius = (0.012 + impact.secondsSinceImpact * 0.20) * dropSize;
        float splashWidth = max(0.008 * dropSize, pixelWidth);
        float splashFade = (1.0 - splashLife) * (1.0 - splashLife);
        roadSplash += exp(-pow((d - splashRadius) / splashWidth, 2.0)) *
                      (0.008 * dropSize / splashWidth) * splashFade * impactEnabled;
    }
    return vec4(slope, min(rings, 1.0), min(roadSplash, 1.0));
}
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    float dropSize = GE_WaterDropScale(Light.uWeatherWindCount.y, Props.waterDropSizeOffset);
    float waterTime = Light.uTimeParams.x * Light.uWeatherWindCount.z;
    vec4 weather = GE_WaterWeather(sIn.positionWS, waterTime);
    vec2 world = sIn.positionRelWS.xz;
    float field = PuddleField(world / max(Props.puddleScale, 0.5));
    if (Props.useMaskTexture)
    {
        vec3 uv = vec3(sIn.uv0, 1);
        uint slot = uint(GE_TEXSLOT_puddleMask);
        vec2 maskUv = vec2(dot(sIn.textureST[slot].xyz, uv), dot(sIn.textureST2[slot].xyz, uv));
        field = texture(GE_USER_TEXTURE(puddleMask), maskUv).r;
    }
    float threshold = 1.0 - clamp(Props.puddleCoverage, 0.0, 1.0);
    float width = max(Props.edgeSoftness, 0.01);
    float puddle = smoothstep(threshold - width, threshold + width, field);
    float damp = smoothstep(threshold - width * 3.0, threshold + width, field);
    if (Props.puddleCoverage <= 0.0) { puddle = 0.0; damp = 0.0; }
    if (Props.puddleCoverage >= 1.0) { puddle = 1.0; damp = 1.0; }

    // Fine aggregate stays in the ground's albedo. Water levels the normal and
    // roughness; it is a dielectric (IOR 1.333), not a metallic mirror.
    float grain = PuddleNoise(world * 37.0) * 0.6 + PuddleNoise(world * 81.0) * 0.4;
    vec3 dry = Props.groundColor * mix(1.0, 0.65 + 0.7 * grain, Props.grainStrength);
    o.baseColor = dry * (1.0 - clamp(Props.wetDarkening, 0.0, 0.95) * damp);
    o.metallic = 0.0;
    o.roughness = mix(Props.groundRoughness, Props.waterRoughness, puddle);
    o.specularIor = mix(1.5, 1.333, puddle);
    vec4 rain = PuddleRain(sIn.positionWS.xz, Light.uTimeParams.x, dropSize, sIn.positionWS.y);
    vec2 microNormal = GE_WaterRippleNormal(world, weather.xz, waterTime,
                                           Props.waterNormalScale * dropSize);
    vec2 slope = (rain.xy * Props.rippleStrength + microNormal * Props.waterNormalStrength) * puddle;
    slope *= min(1.0, 0.35 / max(length(slope), 0.0001));
    o.normalWS = normalize(sIn.normalWS + vec3(-slope.x, 0, -slope.y));
    o.baseColor += vec3(0.14, 0.15, 0.16) * rain.w * (1.0 - puddle);
    o.planarReflectionMask = puddle;
    return o;
}
