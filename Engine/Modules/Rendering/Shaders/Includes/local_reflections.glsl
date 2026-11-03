#ifndef GE_LOCAL_REFLECTIONS_GLSL
#define GE_LOCAL_REFLECTIONS_GLSL

// Shared by forward IBL and the screen-space reflection replacement estimate.
// Explicit samplers keep the two-probe tier independent of descriptor indexing.
layout(set = 0, binding = 51) uniform samplerCube ge_localIrradiance0;
layout(set = 0, binding = 52) uniform samplerCube ge_localPrefilter0;
layout(set = 0, binding = 53) uniform samplerCube ge_localIrradiance1;
layout(set = 0, binding = 54) uniform samplerCube ge_localPrefilter1;

vec3 GE_ProbeLocalPosition(GE_LocalProbeData probe, vec3 positionWS)
{
    vec3 delta = positionWS - probe.centerBlend.xyz;
    return vec3(dot(delta, probe.axisX.xyz), dot(delta, probe.axisY.xyz), dot(delta, probe.axisZ.xyz));
}

float GE_ProbeInfluence(GE_LocalProbeData probe, vec3 positionWS)
{
    if (probe.control.y == 0u) return 0.0;
    vec3 halfExtents = probe.halfExtentsProjection.xyz;
    if (any(lessThanEqual(halfExtents, vec3(1e-4)))) return 0.0;
    vec3 local = GE_ProbeLocalPosition(probe, positionWS);
    float edge;
    if (probe.control.z == 1u)
    {
        vec3 distances = halfExtents - abs(local);
        edge = min(distances.x, min(distances.y, distances.z));
    }
    else
    {
        float radius = length(local / halfExtents);
        if (radius > 1.0) return 0.0;
        edge = radius > 1e-4 ? length(local) * (1.0 / radius - 1.0)
                            : min(halfExtents.x, min(halfExtents.y, halfExtents.z));
    }
    if (edge < 0.0) return 0.0;
    float blend = min(probe.centerBlend.w, min(halfExtents.x, min(halfExtents.y, halfExtents.z)));
    return blend > 1e-4 ? clamp(edge / blend, 0.0, 1.0) : 1.0;
}

// Equal-priority probes share coverage without doubling energy. Higher priority
// consumes its own coverage first and the lower tier fills the remainder, so
// entering a high-priority blend shell is continuous. z is the sky remainder.
vec3 GE_LocalProbeWeights(vec3 positionWS)
{
    float a = GE_ProbeInfluence(Env.localProbes[0], positionWS);
    float b = GE_ProbeInfluence(Env.localProbes[1], positionWS);
    if (Env.localProbes[0].control.x > Env.localProbes[1].control.x) b *= 1.0 - a;
    else if (Env.localProbes[1].control.x > Env.localProbes[0].control.x) a *= 1.0 - b;
    float total = a + b;
    return vec3(a, b, max(0.0, 1.0 - total)) / max(1.0, total);
}

vec3 GE_ProbeReflectionDirection(GE_LocalProbeData probe, vec3 positionWS, vec3 directionWS)
{
    vec3 R = normalize(directionWS);
    if (probe.halfExtentsProjection.w < 0.5) return R;
    vec3 delta = positionWS - probe.projectionCenter.xyz;
    vec3 local = vec3(dot(delta, probe.axisX.xyz), dot(delta, probe.axisY.xyz), dot(delta, probe.axisZ.xyz));
    // Outside the projection room, fall back to the unprojected direction.
    if (any(greaterThan(abs(local), probe.projectionHalfExtents.xyz))) return R;
    vec3 localR = vec3(dot(R, probe.axisX.xyz), dot(R, probe.axisY.xyz), dot(R, probe.axisZ.xyz));
    vec3 safeR = mix(vec3(-1.0), vec3(1.0), greaterThanEqual(localR, vec3(0.0))) * max(abs(localR), vec3(1e-5));
    vec3 farT = max((-probe.projectionHalfExtents.xyz - local) / safeR,
                   ( probe.projectionHalfExtents.xyz - local) / safeR);
    float distance = min(farT.x, min(farT.y, farT.z));
    vec3 hitWS = positionWS + R * max(distance, 0.0);
    vec3 corrected = hitWS - probe.positionIntensity.xyz;
    return dot(corrected, corrected) > 1e-8 ? normalize(corrected) : R;
}

float GE_ProbeRadianceScale(GE_LocalProbeData probe, vec3 direction, float groundParticipation)
{
    float ground = smoothstep(0.0, 0.25, -normalize(direction).y);
    return probe.positionIntensity.w * (1.0 - ground * probe.params.y * clamp(groundParticipation, 0.0, 1.0));
}

vec3 GE_BlendLocalPrefilter(vec3 positionWS, vec3 directionWS, float roughness, float groundParticipation, vec3 sky)
{
    vec3 weights = GE_LocalProbeWeights(positionWS);
    vec3 radiance = sky * weights.z;
    if (weights.x > 0.0)
    {
        vec3 dir = GE_ProbeReflectionDirection(Env.localProbes[0], positionWS, directionWS);
        radiance += weights.x * textureLod(ge_localPrefilter0, dir, roughness * Env.localProbes[0].params.x).rgb
                  * GE_ProbeRadianceScale(Env.localProbes[0], dir, groundParticipation);
    }
    if (weights.y > 0.0)
    {
        vec3 dir = GE_ProbeReflectionDirection(Env.localProbes[1], positionWS, directionWS);
        radiance += weights.y * textureLod(ge_localPrefilter1, dir, roughness * Env.localProbes[1].params.x).rgb
                  * GE_ProbeRadianceScale(Env.localProbes[1], dir, groundParticipation);
    }
    return radiance;
}

vec3 GE_BlendLocalIrradiance(vec3 positionWS, vec3 directionWS, float groundParticipation, vec3 sky)
{
    vec3 weights = GE_LocalProbeWeights(positionWS);
    vec3 radiance = sky * weights.z;
    if (weights.x > 0.0)
        radiance += weights.x * textureLod(ge_localIrradiance0, normalize(directionWS), 0.0).rgb
                  * GE_ProbeRadianceScale(Env.localProbes[0], directionWS, groundParticipation);
    if (weights.y > 0.0)
        radiance += weights.y * textureLod(ge_localIrradiance1, normalize(directionWS), 0.0).rgb
                  * GE_ProbeRadianceScale(Env.localProbes[1], directionWS, groundParticipation);
    return radiance;
}

#endif
