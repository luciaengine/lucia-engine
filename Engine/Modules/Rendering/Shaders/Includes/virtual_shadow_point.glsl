#ifndef GE_VIRTUAL_SHADOW_POINT_GLSL
#define GE_VIRTUAL_SHADOW_POINT_GLSL

#include "virtual_shadow_projections.glsl"

bool GE_VirtualPointShadowTexel(uint row, uint mip, vec2 uv, float reference, out float visibility)
{
    visibility = 0.0;
    if (!GE_VirtualShadowHasProjection(row))
        return false;
    uvec3 address = ge_virtualShadowProjections[row].Address.xyz;
    uint baseSide = address.y;
    if (baseSide == 0u || baseSide > 128u || (baseSide & (baseSide - 1u)) != 0u ||
        mip > address.z || mip > uint(findMSB(baseSide)))
        return false;
    uint firstPage = GE_VirtualShadowMipStart(address.x, baseSide, mip);
    if (firstPage < address.x)
        return false;
    uint side = baseSide >> mip;
    float resolution = float(side * kGE_VirtualShadowPageTexels);
    ivec2 texel = ivec2(clamp(floor(uv * resolution), vec2(0.0), vec2(resolution - 1.0)));
    return GE_VirtualShadowCompareTexel(firstPage, side, texel, reference,
        uint(textureSize(ge_virtualShadowDepth, 0).z), visibility);
}

bool GE_VirtualPointShadowVertex(uint baseRow, uint mip, vec3 position, vec3 vertexDirection,
                                 out float visibility)
{
    visibility = 0.0;
    for (int axis = 0; axis < 3; ++axis)
    {
        uint row = baseRow + uint(axis * 2 + (vertexDirection[axis] >= 0.0 ? 0 : 1));
        if (!GE_VirtualShadowHasProjection(row))
            return false;
        mat4 projection = ge_virtualShadowProjections[row].RelativeToLightClip;
        vec4 clip = projection * vec4(position, 1.0);
        vec4 vertexClip = projection * vec4(vertexDirection, 0.0);
        if (!(clip.w > 1e-6 && vertexClip.w > 1e-6))
            return false;
        vec2 uv = GE_YUpNdcToViewportUV(vertexClip.xy / vertexClip.w);
        float reference = clamp(clip.z / clip.w + 0.0005, 0.0, 1.0);
        float contribution;
        if (!GE_VirtualPointShadowTexel(row, mip, uv, reference, contribution))
            return false;
        visibility += contribution;
    }
    visibility /= 3.0;
    return true;
}

bool GE_VirtualPointShadowFootprint(uint baseRow, int face, uint mip, mat4 projection,
    vec3 position, vec3 lightToPosition, vec2 tapUV, float reference, vec2 texelUV,
    float faceDepth, out float visibility)
{
    bvec2 outside = notEqual(texelUV, clamp(texelUV, 0.0, 1.0));
    if (!any(outside))
        return GE_VirtualPointShadowTexel(baseRow + uint(face), mip, texelUV, reference, visibility);

    vec3 right = vec3(projection[0].x, projection[1].x, projection[2].x);
    vec3 up = vec3(projection[0].y, projection[1].y, projection[2].y);
    vec2 offset = texelUV - tapUV;
    vec3 worldOffset = (right * offset.x - up * offset.y) * (2.0 * faceDepth);
    vec3 texelPosition = position + worldOffset;
    vec3 texelDirection = lightToPosition + worldOffset;
    if (all(outside))
        return GE_VirtualPointShadowVertex(baseRow, mip, texelPosition, sign(texelDirection), visibility);

    uint row = baseRow + uint(GE_PointShadowFace(texelDirection));
    if (!GE_VirtualShadowHasProjection(row))
        return false;
    vec4 clip = ge_virtualShadowProjections[row].RelativeToLightClip * vec4(texelPosition, 1.0);
    if (!(clip.w > 1e-6))
        return false;
    vec2 uv = GE_YUpNdcToViewportUV(clip.xy / clip.w);
    float neighborReference = clamp(clip.z / clip.w + 0.0005, 0.0, 1.0);
    return GE_VirtualPointShadowTexel(row, mip, uv, neighborReference, visibility);
}

bool GE_VirtualPointShadowTap(uint baseRow, int face, uint mip, vec3 position,
    vec3 lightToPosition, vec2 tapUV, float reference, out float visibility)
{
    visibility = 0.0;
    if (!GE_ViewportUVInside(tapUV))
    {
        face = GE_PointShadowFace(lightToPosition);
        uint row = baseRow + uint(face);
        if (!GE_VirtualShadowHasProjection(row))
            return false;
        vec4 clip = ge_virtualShadowProjections[row].RelativeToLightClip * vec4(position, 1.0);
        if (!(clip.w > 1e-6))
            return false;
        tapUV = GE_YUpNdcToViewportUV(clip.xy / clip.w);
        reference = clamp(clip.z / clip.w + 0.0005, 0.0, 1.0);
    }
    GE_VirtualShadowProjection projection = ge_virtualShadowProjections[baseRow + uint(face)];
    uint side = projection.Address.y;
    if (side == 0u || side > 128u || (side & (side - 1u)) != 0u ||
        mip > projection.Address.z || mip > uint(findMSB(side)))
        return false;
    float resolution = float((side >> mip) * kGE_VirtualShadowPageTexels);
    float halfTexel = 0.5 / resolution;
    if (all(greaterThanEqual(tapUV, vec2(halfTexel))) &&
        all(lessThanEqual(tapUV, vec2(1.0 - halfTexel))))
        return GE_VirtualShadowCompareBilinear(projection.Address.xyz, mip, tapUV, reference, visibility);

    float faceDepth = (projection.RelativeToLightClip * vec4(position, 1.0)).w;
    vec2 texelPosition = tapUV * resolution - 0.5;
    vec2 base = floor(texelPosition);
    vec2 fraction = fract(texelPosition);
    for (int vertical = 0; vertical < 2; ++vertical)
        for (int horizontal = 0; horizontal < 2; ++horizontal)
        {
            vec2 corner = vec2(horizontal, vertical);
            vec2 weights = mix(1.0 - fraction, fraction, corner);
            float weight = weights.x * weights.y;
            if (weight <= 0.0)
                continue;
            float contribution;
            vec2 texelUV = (base + corner + 0.5) / resolution;
            if (!GE_VirtualPointShadowFootprint(baseRow, face, mip, projection.RelativeToLightClip,
                position, lightToPosition, tapUV, reference, texelUV, faceDepth, contribution))
                return false;
            visibility += weight * contribution;
        }
    return true;
}

#endif
