// Surface shader: an impostor card that samples its baked atlas (Engine/Rendering/Impostors).
// albedoMap holds base colour and opacity, normalMap the object normal * 0.5 + 0.5 and the depth
// along the frame axis. custom0 of the instance: xy the octahedral code of its view direction,
// zw the cosine and sine of its yaw, which turn the object normal into world space. The card blends
// the three frames around the view direction at the surface point that the baked depth gives.
//
// @texture albedoMap srgb
// @texture normalMap linear
// @texture impostorSeasonMaskMap linear
// @texture impostorDiffuseTransmissionMap linear
// @property vec4 impostorAutumnTint "Impostor Autumn Tint" default=1,1,1,0 group=Impostor
// @property float impostorSnowAmount "Impostor Snow Amount" default=0 group=Impostor
// @property vec4 impostorAtlas "Impostor Atlas" default=8,0,0,0 group=Impostor
// impostorAtlas.z = 1 tints cards red. The w lane is the highest mip available for tile sampling.

#include "Includes/impostor_frame.glsl"
#include "Includes/foliage_coverage.glsl"
#include "Includes/foliage_season.glsl"

// The object-space surface point (in sphere radii from the center) that `frame` sees through card
// point `offset`. The bake camera is orthographic and reverse-Z, with near at 1 radius in front of
// the center and far at 1 radius behind it, so window depth d is at 2d - 1 radii along the frame
// direction. The atlas holds depth premultiplied by coverage; a texel with no surface keeps `offset`.
vec3 GE_ImpostorSurfacePoint(vec2 frame, GE_ImpostorFrameBasis basis, vec3 offset, vec4 impostorAtlas, vec2 halfTexel)
{
    vec2 uv = GE_ImpostorFrameUv(frame, basis, offset, impostorAtlas, halfTexel);
    float coverage = textureLod(albedoMap, uv, 0.0).a;
    vec3 direction = basis.direction;
    float depth = textureLod(normalMap, uv, 0.0).w / max(coverage, 0.0001);
    vec3 surface = offset - direction * dot(offset, direction) + direction * (2.0 * depth - 1.0);
    // An edge texel blends toward the card point, so the point does not jump at a silhouette.
    return mix(offset, surface, smoothstep(0.25, 0.75, coverage));
}

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    // The card point, from the sphere center in sphere radii, in object space.
    vec3 right;
    vec3 up;
    GE_ImpostorFrameAxes(GE_ImpostorDecode(sIn.custom0.xy, Props.impostorAtlas), right, up);
    vec2 corner = vec2(sIn.uv0.x * 2.0 - 1.0, 1.0 - sIn.uv0.y * 2.0);
    vec3 offset = right * corner.x + up * corner.y;

    vec2 frames[3];
    vec3 weights;
    GE_ImpostorBlendFrames(sIn.custom0.xy, Props.impostorAtlas, frames, weights);
    vec2 frameTexels = vec2(textureSize(albedoMap, 0)) / max(Props.impostorAtlas.x, 1.0);
    vec2 dx = dFdx(sIn.uv0) * frameTexels;
    vec2 dy = dFdy(sIn.uv0) * frameTexels;
    float footprint = max(dot(dx, dx), dot(dy, dy));
    float mip = min(max(0.0, Props.impostorAtlas.w), 0.5 * log2(max(1.0, footprint)));
    vec2 halfTexel = min(vec2(0.5), 0.5 / frameTexels);
    // Trilinear taps at both levels must stay inside this view tile.
    vec2 shadingInset = min(vec2(0.5), halfTexel * exp2(ceil(mip)));

    // Atlas texels outside the tree hold 0 in every channel, so a filtered sample at a leaf edge is
    // already premultiplied by its coverage. Divide by the summed coverage; never multiply by it.
    // The surface point: each frame's depth under this card point, blended by the frame weights, so
    // the point moves smoothly when the view crosses from one frame to the next. The frames then
    // sample that point, so their texels line up instead of each showing the card plane.
    vec3 surface = vec3(0.0);
    GE_ImpostorFrameBasis frameBases[3];
    for (int i = 0; i < 3; ++i)
    {
        frameBases[i] = GE_ImpostorBuildFrameBasis(frames[i], Props.impostorAtlas);
        surface += GE_ImpostorSurfacePoint(frames[i], frameBases[i], offset, Props.impostorAtlas, halfTexel) * weights[i];
    }

    // A frame that has a surface in front of the surface point sees an occluder there (the leaves
    // above a trunk, seen from a higher frame). Its weight fades out over kOccluderFade, down to
    // kOccludedWeight, so no frame drops out in one step and the weights never sum to 0.
    const float kOccluderDepth = 0.05;  // Window depth: 0.1 sphere radii.
    const float kOccluderFade = 0.05;
    const float kOccludedWeight = 0.02;
    vec4 color = vec4(0.0);
    vec3 objectNormal = vec3(0.0);
    float weightSum = 0.0;
#if !defined(GE_DEPTH_ONLY_FRAGMENT) && !defined(GE_MOTION_VECTORS)
    vec4 autumnTint = Light.uFoliageAutumnTint.a > 0.0 ? Light.uFoliageAutumnTint : Props.impostorAutumnTint;
    float snowAmount = max(Light.uFoliageSnow.x, Props.impostorSnowAmount);
    bool shadeSeason = autumnTint.a > 0.0 || snowAmount > 0.0;
    vec2 seasonMask = vec2(0.0);
  #ifdef GE_DIFFUSE_TRANSMISSION_ENABLED
    vec4 diffuseTransmission = vec4(0.0);
  #endif
#endif
    for (int i = 0; i < 3; ++i)
    {
        vec2 uv = GE_ImpostorFrameUv(frames[i], frameBases[i], surface, Props.impostorAtlas, shadingInset);
        vec4 sampleColor = textureLod(albedoMap, uv, mip);
        vec4 sampleNormalDepth = textureLod(normalMap, uv, mip);
        float expectedDepth = dot(surface, frameBases[i].direction) * 0.5 + 0.5;
        float storedDepth = sampleNormalDepth.w / max(sampleColor.a, 0.0001);
        float occluded = smoothstep(kOccluderDepth, kOccluderDepth + kOccluderFade, storedDepth - expectedDepth) *
                         smoothstep(0.25, 0.75, sampleColor.a) * (1.0 - smoothstep(1.0, 3.0, mip));
        float weight = weights[i] * max(1.0 - occluded, kOccludedWeight);
        color += sampleColor * weight;
        objectNormal += (sampleNormalDepth.xyz * 2.0 - sampleColor.a) * weight;
        weightSum += weight;
#if !defined(GE_DEPTH_ONLY_FRAGMENT) && !defined(GE_MOTION_VECTORS)
        if (shadeSeason)
            seasonMask += textureLod(GE_USER_TEXTURE(impostorSeasonMaskMap), uv, mip).rg * weight;
  #ifdef GE_DIFFUSE_TRANSMISSION_ENABLED
        diffuseTransmission += textureLod(GE_USER_TEXTURE(impostorDiffuseTransmissionMap), uv, mip) * weight;
  #endif
#endif
    }
    color /= weightSum;
    objectNormal /= weightSum;
    o.baseColor = color.a > 0.0001 ? color.rgb / color.a : vec3(0.0);
    o.opacity = color.a;
    if (mip > 0.5)
    {
        float cutout = step(clamp(Props.alphaCutoff, 0.0, 1.0), color.a);
        float coverage = mix(cutout, color.a, smoothstep(0.5, 2.0, mip));
        o.opacity = coverage > GE_FoliageScreenThreshold(gl_FragCoord.xy) ? 1.0 : 0.0;
    }
    if (Props.impostorAtlas.z > 0.5)
        o.baseColor = mix(o.baseColor, vec3(1.0, 0.15, 0.1), 0.65);

    float c = sIn.custom0.z;
    float s = sIn.custom0.w;
    objectNormal = normalize(objectNormal + vec3(0.0, 1e-4, 0.0));
    o.normalWS = normalize(vec3(c * objectNormal.x + s * objectNormal.z, objectNormal.y,
                                -s * objectNormal.x + c * objectNormal.z));
    o.roughness = 0.9;
    o.metallic = 0.0;
#if !defined(GE_DEPTH_ONLY_FRAGMENT) && !defined(GE_MOTION_VECTORS)
    if (shadeSeason && color.a > 0.0001)
    {
        // Coverage also filters the mask. Normalize it independently of the weather amounts.
        seasonMask /= weightSum * color.a;
        o.baseColor = GE_ApplyFoliageSeason(o.baseColor, o.normalWS, seasonMask, autumnTint, snowAmount);
    }
  #ifdef GE_DIFFUSE_TRANSMISSION_ENABLED
    if (color.a > 0.0001 && diffuseTransmission.a > 0.0001)
    {
        o.diffuseTransmission = clamp(diffuseTransmission.a / (weightSum * color.a), 0.0, 1.0);
        o.diffuseTransmissionAlbedo = diffuseTransmission.rgb / diffuseTransmission.a;
    }
  #endif
#endif
    return o;
}
