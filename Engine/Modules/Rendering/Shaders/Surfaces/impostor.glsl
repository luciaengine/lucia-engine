// Surface shader: an impostor card that samples its baked atlas (Engine/Rendering/Impostors).
// albedoMap holds base colour and opacity, normalMap the object normal * 0.5 + 0.5 and the depth
// along the frame axis. custom0 of the instance: xy the octahedral code of its view direction,
// zw the cosine and sine of its yaw, which turn the object normal into world space. The card blends
// the three frames around the view direction, each reprojected onto the card.
//
// @property vec4 impostorAtlas "Impostor Atlas" default=8,0,0,0 group=Impostor
// impostorAtlas.z = 1 tints the cards red (GE_IMPOSTOR_DEBUG_TINT), to show which trees are cards.

#include "Includes/impostor_frame.glsl"

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
    vec4 color = vec4(0.0);
    vec3 objectNormal = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        vec2 uv = GE_ImpostorFrameUv(frames[i], offset, Props.impostorAtlas);
        vec4 sampleColor = texture(albedoMap, uv);
        // Colour weighted by opacity, so a frame's empty texels do not darken the others.
        color.rgb += sampleColor.rgb * sampleColor.a * weights[i];
        color.a += sampleColor.a * weights[i];
        objectNormal += (texture(normalMap, uv).xyz * 2.0 - 1.0) * sampleColor.a * weights[i];
    }
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
