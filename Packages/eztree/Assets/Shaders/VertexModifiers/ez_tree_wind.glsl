// EZ Tree wind vertex modifier.
// Native C++/GLSL integration derived from EZ-Tree authored wind options.
// EZ-Tree upstream: MIT License, Copyright (c) 2024 Daniel Greenheck.
//
// Culling envelope: with W = length(windStrength.xyz), each term below is bounded
// by its largest weight, with no assumption about the directions it adds along:
// trunk .32 W, branch .35 W, flutter .34 x 1.45 W along windDir + .45 lateralDir,
// lift .12 x .34 W. The sum is 1.204 W. Wind volumes can make the wind stronger
// than W; the tree extraction adds that bound to each instance's culling sphere.
// @deformationEnvelope 1.21 scaledBy=windStrength

#include "Includes/weather_wind_resolve.glsl"

float EZTreeWindHash(vec3 p)
{
    return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

vec3 ModifyVertex(vec3 position, InstanceData inst)
{
    // Mat.uParams1: xyz = authored wind in tree space times the part's strength scale,
    // w = that strength scale, 0 when the tree has no wind.
    // Mat.uParams2: x = authored gust frequency, y = authored spatial scale,
    // z = tree height, w = base Y. All are per tree variant: the wind volumes at the
    // instance position make the wind of each tree.
    float strengthScale = Mat.uParams1.w;
    if (strengthScale < 0.0001)
        return position;

    float heightScale = max(Mat.uParams2.z, 0.001);
    float height01 = clamp((position.y - Mat.uParams2.w) / heightScale, 0.0, 1.0);
    float trunkWeight = height01 * height01 * (3.0 - 2.0 * height01);
    float branchWeight = pow(height01, 1.35);

    // The authored wind turns and scales with the tree; the volumes blend over it in
    // world space, as WindVolumeResolver::ResolveAt does on the CPU.
    mat3 basis = mat3(inst.modelMatrix);
    GE_ResolvedWind baseWind = GE_DefaultWind();
    baseWind.velocity = basis * (Mat.uParams1.xyz / strengthScale);
    baseWind.frequency = max(Mat.uParams2.x, 0.0);
    baseWind.gustScale = max(Mat.uParams2.y, 0.001);
    vec3 treeWorld = vec3(inst.sector) * GE_SECTOR_SIZE + inst.modelMatrix[3].xyz;
    GE_ResolvedWind resolved = GE_ResolveWeatherWind(treeWorld, baseWind);
    vec3 wind = vec3(dot(resolved.velocity, normalize(basis[0])),
                     dot(resolved.velocity, normalize(basis[1])),
                     dot(resolved.velocity, normalize(basis[2]))) * strengthScale;

    float time = inst.deformationTimeSeconds * resolved.frequency;
    float spatialScale = resolved.gustScale;
    float spatial = (position.x * 0.21 + position.y * 0.13 + position.z * 0.17) / spatialScale;
    // Every seed is the same at every vertex of a leaf card, so a card's corners never move with
    // unrelated phases and tear into a sliver; the card still bends smoothly with the position terms
    // and the edge flutter.
    // The tree's seed phases the branch-scale sway (slowGust, secondaryWave) of the bark and of every leaf, so a
    // leaf sways with the twig it hangs from. The leaf's own seed, the same at every vertex of its cards (UV1.x,
    // written by the generator), phases only its fine ripple; bark carries no UV1 and ripples with the tree's seed.
    float treeSeed = EZTreeWindHash(vec3(float(inst.instanceIndex) * 0.031));
#ifdef HAS_UV1
    float leafSeed = fract(aUV1.x + float(inst.instanceIndex) * 0.031);
#else
    float leafSeed = treeSeed;
#endif

#ifdef HAS_UV0
    vec2 leafUv = fract(aUV0);
    vec2 fromCenter = abs(leafUv - vec2(0.5)) * 2.0;
    float edgeFlutter = smoothstep(0.25, 0.95, max(fromCenter.x, fromCenter.y));
#else
    float edgeFlutter = 0.35;
#endif

    float windMagnitude = length(wind);
    if (windMagnitude <= 0.0001)
        return position;

    vec3 windDir = wind / windMagnitude;
    vec3 lateralDir = normalize(vec3(-windDir.z, 0.0, windDir.x) + vec3(0.0001, 0.0, 0.0));

    float slowGust = sin(time * 0.37 + spatial * 0.55 + treeSeed * 6.2831);
    float mainWave = sin(time + spatial + slowGust * 0.45);
    float secondaryWave = sin(time * 1.91 + position.x * 0.19 - position.z * 0.23 + treeSeed * 12.566);
    float ripple = sin(time * 5.2 + position.y * 0.61 + leafSeed * 18.849);

    float trunkSway = (mainWave * 0.72 + secondaryWave * 0.28) * trunkWeight * 0.32;
    float branchSway = (secondaryWave * 0.55 + slowGust * 0.45) * branchWeight * 0.35;
    float leafFlutter = ripple * edgeFlutter * branchWeight * 0.34;

    vec3 displacement = wind * trunkSway;
    displacement += lateralDir * windMagnitude * branchSway;
    displacement += (windDir + lateralDir * 0.45) * windMagnitude * leafFlutter;
    displacement.y += windMagnitude * leafFlutter * 0.12;

    return position + displacement;
}
