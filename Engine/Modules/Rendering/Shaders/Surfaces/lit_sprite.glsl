// Lit 2D sprite: albedo, normal and emissive atlas transforms are independent.
// @texture albedoMap srgb
// @texture normalMap
// @texture emissiveMap srgb
#include "Includes/sprite_lighting.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    vec2 uv = GE_TransformUV(sIn.uv0, sIn.textureST[0], sIn.textureST2[0]);
    vec2 normalUV = GE_TransformUV(sIn.uv0, sIn.textureST[1], sIn.textureST2[1]);
    vec2 emissiveUV = GE_TransformUV(sIn.uv0, sIn.textureST[3], sIn.textureST2[3]);
    // Evaluate derivatives before alpha clipping in the adapter.
    vec3 paintedNormal = GE_DecodeTangentNormal(texture(normalMap, normalUV));
    o.normalWS = GE_SpriteNormalFromDerivatives(sIn.normalWS,
        dFdx(sIn.positionWS), dFdy(sIn.positionWS),
        dFdx(normalUV), dFdy(normalUV), paintedNormal);
    vec4 albedo = texture(albedoMap, uv);
    o.baseColor = albedo.rgb * Mat.uBaseColor.rgb * sIn.vertexColor.rgb;
    o.opacity = albedo.a * Mat.uBaseColor.a * sIn.vertexColor.a;
    o.roughness = 1.0;
    o.metallic = 0.0;
    o.specularWeight = 0.0;
    o.emissive = (texture(emissiveMap, emissiveUV).rgb + Mat.uParams18.rgb) *
                 (Mat.uParams18.w / GE_EMISSION_PAPERWHITE_NITS);
    return o;
}
