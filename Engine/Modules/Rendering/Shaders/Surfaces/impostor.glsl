// Surface shader: an impostor card that samples its baked atlas (Engine/Rendering/Impostors).
// albedoMap holds base colour and opacity, normalMap the object normal * 0.5 + 0.5 and the depth
// along the frame axis. custom0 of the instance: xy the octahedral code of its view direction,
// zw the cosine and sine of its yaw, which turn the object normal into world space. The card blends
// the three frames around the view direction at the surface point that the baked depth gives.
//
// @property vec4 impostorAtlas "Impostor Atlas" default=9,0,0,0 group=Impostor
// impostorAtlas.z = 1 tints the cards red (GE_IMPOSTOR_DEBUG_TINT), to show which trees are cards.

#include "Includes/impostor_frame.glsl"

// The object-space surface point (in sphere radii from the center) that `frame` sees through card
// point `offset`. The bake camera is orthographic and reverse-Z, with near at 1 radius in front of
// the center and far at 1 radius behind it, so window depth d is at 2d - 1 radii along the frame
// direction. The atlas holds depth premultiplied by coverage; a texel with no surface keeps `offset`.
vec3 GE_ImpostorSurfacePoint(vec2 frame, vec3 offset, vec4 impostorAtlas)
{
    vec4 sampleColor = texture(albedoMap, GE_ImpostorFrameUv(frame, offset, impostorAtlas));
    float coverage = sampleColor.a;
    if (coverage < 0.5)
        return offset;
    vec3 direction = GE_ImpostorFrameDirection(frame, impostorAtlas);
    float depth = texture(normalMap, GE_ImpostorFrameUv(frame, offset, impostorAtlas)).w / coverage;
    return offset - direction * dot(offset, direction) + direction * (2.0 * depth - 1.0);
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

    // Atlas texels outside the tree hold 0 in every channel, so a filtered sample at a leaf edge is
    // already premultiplied by its coverage. Divide by the summed coverage; never multiply by it.
    // The surface point: the depth of the dominant frame under this card point. The other frames
    // sample that point, so their texels line up with the dominant frame instead of the card plane.
    int dominant = weights.x >= weights.y ? (weights.x >= weights.z ? 0 : 2) : (weights.y >= weights.z ? 1 : 2);
    vec3 surface = GE_ImpostorSurfacePoint(frames[dominant], offset, Props.impostorAtlas);

    // A frame that has a surface clearly in front of the surface point sees an occluder there (the
    // leaves above a trunk, seen from a higher frame): it does not contribute.
    const float kOccluderDepth = 0.05; // Window depth: 0.1 sphere radii.
    vec4 color = vec4(0.0);
    vec3 objectNormal = vec3(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        vec2 uv = GE_ImpostorFrameUv(frames[i], surface, Props.impostorAtlas);
        vec4 sampleColor = texture(albedoMap, uv);
        vec4 sampleNormalDepth = texture(normalMap, uv);
        float expectedDepth = dot(surface, GE_ImpostorFrameDirection(frames[i], Props.impostorAtlas)) * 0.5 + 0.5;
        bool occluded = i != dominant && sampleColor.a > 0.5 &&
                        sampleNormalDepth.w / sampleColor.a > expectedDepth + kOccluderDepth;
        float weight = occluded ? 0.0 : weights[i];
        color += sampleColor * weight;
        objectNormal += (sampleNormalDepth.xyz * 2.0 - sampleColor.a) * weight;
        weightSum += weight;
    }
    color /= weightSum;
    objectNormal /= weightSum;
    o.baseColor = color.a > 0.0001 ? color.rgb / color.a : vec3(0.0);
    o.opacity = color.a;
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
