// Surface shader: an impostor card that samples its baked atlas (Engine/Rendering/Impostors).
// albedoMap holds base colour and coverage, normalMap the object normal * 0.5 + 0.5 and the depth
// along the frame axis. custom0 of the instance: xy the octahedral code of its view direction,
// zw the cosine and sine of its yaw, which turn the object normal into world space.
//
// @property vec4 impostorAtlas "Impostor Atlas" default=8,0,0,0 group=Impostor

#include "Includes/impostor_frame.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    vec2 frame = GE_ImpostorFrame(sIn.custom0.xy, Props.impostorAtlas);
    vec2 atlasUv = (frame + clamp(sIn.uv0, vec2(0.0), vec2(1.0))) / max(Props.impostorAtlas.x, 1.0);

    vec4 color = texture(albedoMap, atlasUv);
    o.baseColor = color.rgb;
    o.opacity = color.a;

    vec3 objectNormal = texture(normalMap, atlasUv).xyz * 2.0 - 1.0;
    float c = sIn.custom0.z;
    float s = sIn.custom0.w;
    o.normalWS = normalize(vec3(c * objectNormal.x + s * objectNormal.z, objectNormal.y,
                                -s * objectNormal.x + c * objectNormal.z));
    o.roughness = 0.9;
    o.metallic = 0.0;
    return o;
}
