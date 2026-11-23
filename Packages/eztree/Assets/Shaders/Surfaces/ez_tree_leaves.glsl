// Surface shader: EZ Tree leaves.
// Native C++ port integration for @dgreenheck/ez-tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.
//
// Texture names (albedoMap, normalMap, metallicRoughnessMap, aoMap) are
// provided by the adapter's bindless slot macros.

#include "Includes/foliage_coverage.glsl"

float GE_EZTreeLeafAlpha(vec4 albedo)
{
    const float kAuthoredAlphaMax = 0.999;
    const float kMissingTextureSaturationMax = 0.02;
    const float kMissingTextureMinChannel = 0.85;
    const float kChromaCutoutLo = 0.035;
    const float kChromaCutoutHi = 0.16;

    // Match upstream EZ-Tree for authored PNG leaves: use texture alpha
    // directly with the material alpha-test cutoff.
    if (albedo.a < kAuthoredAlphaMax)
        return albedo.a;

    float maxChannel = max(max(albedo.r, albedo.g), albedo.b);
    float minChannel = min(min(albedo.r, albedo.g), albedo.b);
    float saturation = maxChannel - minChannel;
    // The material default albedo is 1x1 opaque white. Chroma cutout of that
    // sample discards every card, so a missing bind still draws the tint.
    if (saturation <= kMissingTextureSaturationMax && minChannel >= kMissingTextureMinChannel)
        return 1.0;

    // Some external atlases are opaque RGB sheets. Infer a cutout from
    // leaf-like chroma instead of brightness so white/gray atlas backing does
    // not survive alpha testing.
    float greenDominance = albedo.g - max(albedo.r, albedo.b);
    return smoothstep(kChromaCutoutLo, kChromaCutoutHi, max(saturation * 0.75, greenDominance * 1.5));
}

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    vec2 uvAlbedo = GE_TransformUV(sIn.uv0, sIn.textureST[0], sIn.textureST2[0]);
    vec2 uvMR = GE_TransformUV(sIn.uv0, sIn.textureST[2], sIn.textureST2[2]);
    vec2 uvNormal = GE_TransformUV(sIn.uv0, sIn.textureST[1], sIn.textureST2[1]);

    vec4 albedo = texture(albedoMap, uvAlbedo);
    float leafAlpha = GE_EZTreeLeafAlpha(albedo);
    // A far leaf reads the last levels, where the filtered alpha is the leaf's covered share and is
    // below the cutoff. There the coverage test keeps that share: a leaf passes where its filtered
    // alpha is above its seed (UV1.x, one value in [0, 1) per leaf along the leaf index). The test
    // reads no level coarser than 4 texels across, so a passing leaf keeps a leaf shape, not its
    // card. The pattern moves with the leaf and overlapping leaves pass independently. Color
    // textures filter their mips with alpha-weighted color (TextureCook.h), so the color of the
    // coarse levels is the leaf color.
    vec2 uvDx = dFdx(uvAlbedo);
    vec2 uvDy = dFdy(uvAlbedo);
    float footprint = GE_FoliageFootprint(uvDx, uvDy, GE_MATERIAL_MIP_BIAS);
    float tailWeight = GE_FoliageTailWeight(footprint);
    float shapeAlpha = leafAlpha;
    if (footprint > kFoliageShapeFootprint)
    {
        float shapeScale = kFoliageShapeFootprint / footprint;
        shapeAlpha = GE_EZTreeLeafAlpha(textureGrad(albedoMap, uvAlbedo, uvDx * shapeScale, uvDy * shapeScale));
    }
    float colorTrust = max(smoothstep(0.45, 0.95, leafAlpha), tailWeight);
    vec3 leafColor = mix(Mat.uBaseColor.rgb, albedo.rgb, colorTrust);
    o.baseColor = leafColor * Mat.uBaseColor.rgb;
    o.opacity = GE_FoliageCoverageOpacity(shapeAlpha * Mat.uBaseColor.a, clamp(Props.alphaCutoff, 0.0, 1.0),
                                          tailWeight, sIn.uv1.x);

    vec4 mr = texture(metallicRoughnessMap, uvMR);
    o.roughness = mr.g * Mat.uParams0.y;
    o.metallic = mr.b * Mat.uParams0.x;

    vec2 uvAO = GE_TransformUV(sIn.uv0, sIn.textureST[4], sIn.textureST2[4]);
    o.ao = texture(aoMap, uvAO).r;

#ifdef HAS_TANGENT
    // Z reconstructed — see GE_DecodeTangentNormal (uniform for RGB8 and BC5 payloads).
    vec3 tn = GE_DecodeTangentNormal(texture(normalMap, uvNormal));
    o.normalWS = normalize(sIn.TBN * tn);
#else
    o.normalWS = normalize(sIn.normalWS);
#endif

    return o;
}
