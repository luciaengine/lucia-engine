// Surface shader: an impostor card that samples its baked atlas (Engine/Rendering/Impostors).
// albedoMap holds base colour and opacity, normalMap the object normal * 0.5 + 0.5 and the depth
// along the frame axis. custom0 of the instance: xy the octahedral code of its view direction,
// zw the cosine and sine of its yaw, which turn the object normal into world space. The card blends
// the three frames around the view direction at the surface point that the baked depth gives.
//
// @property vec4 impostorAtlas "Impostor Atlas" default=8,0,0,0 group=Impostor
// impostorAtlas.z enables the debug tint. impostorAtlas.w is the highest ready mip.

#include "Includes/impostor_frame.glsl"
#include "Includes/foliage_coverage.glsl"

vec2 GE_ImpostorClampTile(vec2 atlasUv, vec2 frame, float gridSize, vec2 inset)
{
    vec2 tile = clamp(atlasUv * gridSize - frame, inset, vec2(1.0) - inset);
    return (frame + tile) / gridSize;
}

// The object-space surface point (in sphere radii from the center) that `frame` sees through card
// point `offset`. The bake camera is orthographic and reverse-Z, with near at 1 radius in front of
// the center and far at 1 radius behind it, so window depth d is at 2d - 1 radii along the frame
// direction. The atlas holds depth premultiplied by coverage; a texel with no surface keeps `offset`.
vec3 GE_ImpostorSurfacePoint(vec2 frame, vec3 offset, vec4 impostorAtlas, vec2 halfTexel)
{
    vec2 uv = GE_ImpostorFrameUv(frame, offset, impostorAtlas);
    bool filtered = impostorAtlas.w > 0.0;
    if (filtered)
        uv = GE_ImpostorClampTile(uv, frame, impostorAtlas.x, halfTexel);
    float coverage = filtered ? textureLod(albedoMap, uv, 0.0).a : texture(albedoMap, uv).a;
    vec3 direction = GE_ImpostorFrameDirection(frame, impostorAtlas);
    float storedDepth = filtered ? textureLod(normalMap, uv, 0.0).w : texture(normalMap, uv).w;
    float depth = storedDepth / max(coverage, 0.0001);
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

    bool filtered = Props.impostorAtlas.w > 0.0;
    float gridSize = max(Props.impostorAtlas.x, 1.0);
    vec2 uvDx = dFdx(sIn.uv0);
    vec2 uvDy = dFdy(sIn.uv0);
    float mip = 0.0;
    vec2 halfTexel = vec2(0.0);
    vec2 shadingInset = vec2(0.0);
    if (filtered)
    {
        vec2 tileTexels = vec2(textureSize(albedoMap, 0)) / gridSize;
        vec2 dx = uvDx * tileTexels;
        vec2 dy = uvDy * tileTexels;
        float footprint = max(dot(dx, dx), dot(dy, dy));
        mip = min(Props.impostorAtlas.w, 0.5 * log2(max(1.0, footprint)));
        halfTexel = 0.5 / tileTexels;
        // Keep both trilinear samples inside one view tile.
        shadingInset = halfTexel * exp2(ceil(mip));
    }

    // Atlas texels outside the tree hold 0 in every channel, so a filtered sample at a leaf edge is
    // already premultiplied by its coverage. Divide by the summed coverage; never multiply by it.
    // The surface point: each frame's depth under this card point, blended by the frame weights, so
    // the point moves smoothly when the view crosses from one frame to the next. The frames then
    // sample that point, so their texels line up instead of each showing the card plane.
    vec3 surface = vec3(0.0);
    for (int i = 0; i < 3; ++i)
        surface += GE_ImpostorSurfacePoint(frames[i], offset, Props.impostorAtlas, halfTexel) * weights[i];

    // A frame that has a surface in front of the surface point sees an occluder there (the leaves
    // above a trunk, seen from a higher frame). Its weight fades out over kOccluderFade, down to
    // kOccludedWeight, so no frame drops out in one step and the weights never sum to 0.
    const float kOccluderDepth = 0.05;  // Window depth: 0.1 sphere radii.
    const float kOccluderFade = 0.05;
    const float kOccludedWeight = 0.02;
    vec4 color = vec4(0.0);
    vec3 objectNormal = vec3(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        vec2 uv = GE_ImpostorFrameUv(frames[i], surface, Props.impostorAtlas);
        if (filtered)
            uv = GE_ImpostorClampTile(uv, frames[i], gridSize, shadingInset);
        vec4 sampleColor = filtered ? textureLod(albedoMap, uv, mip) : texture(albedoMap, uv);
        vec4 sampleNormalDepth = filtered ? textureLod(normalMap, uv, mip) : texture(normalMap, uv);
        float expectedDepth = dot(surface, GE_ImpostorFrameDirection(frames[i], Props.impostorAtlas)) * 0.5 + 0.5;
        float storedDepth = sampleNormalDepth.w / max(sampleColor.a, 0.0001);
        float occluded = smoothstep(kOccluderDepth, kOccluderDepth + kOccluderFade, storedDepth - expectedDepth) *
                         smoothstep(0.25, 0.75, sampleColor.a) * (1.0 - smoothstep(1.0, 3.0, mip));
        float weight = weights[i] * max(1.0 - occluded, kOccludedWeight);
        color += sampleColor * weight;
        objectNormal += (sampleNormalDepth.xyz * 2.0 - sampleColor.a) * weight;
        weightSum += weight;
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
    return o;
}
