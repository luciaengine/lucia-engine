#ifndef GE_POINT_SHADOW_SAMPLING_GLSL
#define GE_POINT_SHADOW_SAMPLING_GLSL

#include "compat_profile.glsl"
#include "vogel_disk.glsl"

// Mirrors RenderServices.h PointShadowSlotGPU (416 bytes per atlas slot).
struct PointShadowSlot
{
    mat4 faceVP[6];
    vec4 params;  // x=enabled, y=baseLayer (slot*6), z=depthBias, w=normalBias
    vec4 params2; // x=tileResolution, y=near, z=far, w=tileScale (tileResolution/atlasRes)
};
layout(set = 0, binding = 25, std430) readonly buffer PointShadowData
{
    PointShadowSlot ge_pointShadowSlots[];
};
layout(set = 0, binding = 26) uniform sampler2DArrayShadow ge_pointShadowMap;

int GE_PointShadowFace(vec3 direction)
{
    vec3 absoluteDirection = abs(direction);
    if (absoluteDirection.x >= absoluteDirection.y && absoluteDirection.x >= absoluteDirection.z)
        return direction.x >= 0.0 ? 0 : 1;
    if (absoluteDirection.y >= absoluteDirection.x && absoluteDirection.y >= absoluteDirection.z)
        return direction.y >= 0.0 ? 2 : 3;
    return direction.z >= 0.0 ? 4 : 5;
}

float GE_ComparePointShadowTexel(int slot, int face, vec2 faceUV, float referenceDepth)
{
    vec4 tileParameters = ge_pointShadowSlots[slot].params2;
    float resolution = max(tileParameters.x, 1.0);
    vec2 texel = clamp(floor(faceUV * resolution), vec2(0.0), vec2(resolution - 1.0));
    vec2 atlasUV = (texel + 0.5) * (tileParameters.w / resolution);
    float layer = ge_pointShadowSlots[slot].params.y + float(face);
    return GE_SHADOW_TAP(ge_pointShadowMap, vec4(atlasUV, layer, referenceDepth));
}

float GE_ComparePointShadowVertex(int slot, vec3 position, vec3 vertexDirection)
{
    // A cube vertex belongs to three faces. Give its virtual corner texel
    // equal weight from each face, with a separate projective depth reference.
    float visibility = 0.0;
    for (int axis = 0; axis < 3; ++axis)
    {
        int face = axis * 2 + (vertexDirection[axis] >= 0.0 ? 0 : 1);
        mat4 faceVP = ge_pointShadowSlots[slot].faceVP[face];
        vec4 clip = faceVP * vec4(position, 1.0);
        vec4 vertexClip = faceVP * vec4(vertexDirection, 0.0);
        vec2 faceUV = vertexClip.xy / vertexClip.w * vec2(0.5, -0.5) + 0.5;
        float referenceDepth = clamp(clip.z / clip.w + ge_pointShadowSlots[slot].params.z, 0.0, 1.0);
        visibility += GE_ComparePointShadowTexel(slot, face, faceUV, referenceDepth);
    }
    return visibility / 3.0;
}

float GE_PointShadowFootprintTexel(int slot, int face, mat4 faceVP, vec3 position,
    vec3 lightToPosition, vec2 tapUV, float referenceDepth, vec2 texelUV, float faceDepth)
{
    bvec2 outside = notEqual(texelUV, clamp(texelUV, 0.0, 1.0));
    if (!any(outside))
        return GE_ComparePointShadowTexel(slot, face, texelUV, referenceDepth);

    // Extend the tap's selected face-depth plane to the virtual texel center. Snapping
    // the destination lookup does not move this comparison point.
    vec3 right = vec3(faceVP[0].x, faceVP[1].x, faceVP[2].x);
    vec3 up = vec3(faceVP[0].y, faceVP[1].y, faceVP[2].y);
    vec2 offset = texelUV - tapUV;
    vec3 worldOffset = (right * offset.x - up * offset.y) * (2.0 * faceDepth);
    vec3 texelPosition = position + worldOffset;
    vec3 texelDirection = lightToPosition + worldOffset;
    if (all(outside))
        return GE_ComparePointShadowVertex(slot, texelPosition, sign(texelDirection));

    int neighbor = GE_PointShadowFace(texelDirection);
    vec4 clip = ge_pointShadowSlots[slot].faceVP[neighbor] * vec4(texelPosition, 1.0);
    vec2 neighborUV = clip.xy / clip.w * vec2(0.5, -0.5) + 0.5;
    float neighborDepth = clamp(clip.z / clip.w + ge_pointShadowSlots[slot].params.z, 0.0, 1.0);
    return GE_ComparePointShadowTexel(slot, neighbor, neighborUV, neighborDepth);
}

float GE_PointShadowTap(int slot, int face, vec3 position, vec3 lightToPosition,
                        vec2 tapUV, float referenceDepth)
{
    vec4 parameters = ge_pointShadowSlots[slot].params;
    vec4 tileParameters = ge_pointShadowSlots[slot].params2;
    if (any(notEqual(tapUV, clamp(tapUV, 0.0, 1.0))))
    {
        face = GE_PointShadowFace(lightToPosition);
        vec4 clip = ge_pointShadowSlots[slot].faceVP[face] * vec4(position, 1.0);
        tapUV = clip.xy / clip.w * vec2(0.5, -0.5) + 0.5;
        referenceDepth = clamp(clip.z / clip.w + parameters.z, 0.0, 1.0);
    }
    float resolution = max(tileParameters.x, 1.0);
    float halfTexel = 0.5 / resolution;
    if (all(greaterThanEqual(tapUV, vec2(halfTexel))) &&
        all(lessThanEqual(tapUV, vec2(1.0 - halfTexel))))
        return GE_SHADOW_TAP(ge_pointShadowMap,
            vec4(tapUV * tileParameters.w, parameters.y + float(face), referenceDepth));

    // Expand only edge footprints. Interior taps retain hardware bilinear PCF.
    mat4 faceVP = ge_pointShadowSlots[slot].faceVP[face];
    float faceDepth = (faceVP * vec4(position, 1.0)).w;
    vec2 texelPosition = tapUV * resolution - 0.5;
    vec2 base = floor(texelPosition);
    vec2 fraction = fract(texelPosition);
    float visibility = 0.0;
    for (int vertical = 0; vertical < 2; ++vertical)
        for (int horizontal = 0; horizontal < 2; ++horizontal)
        {
            vec2 corner = vec2(horizontal, vertical);
            vec2 weights = mix(1.0 - fraction, fraction, corner);
            float weight = weights.x * weights.y;
            if (weight > 0.0)
            {
                vec2 texelUV = (base + corner + 0.5) / resolution;
                visibility += weight * GE_PointShadowFootprintTexel(slot, face, faceVP, position,
                    lightToPosition, tapUV, referenceDepth, texelUV, faceDepth);
            }
        }
    return visibility;
}

vec2 GE_PointShadowRotation(vec2 shadowUV, float face, float tileResolution)
{
    // The tile resolution sets the Vogel rotation seed's grid frequency.
    vec2 shadowTexel = floor(clamp(shadowUV, vec2(0.0), vec2(0.999999)) * max(tileResolution, 1.0));
    shadowTexel += vec2(face * 17.0, face * 31.0);
    float angle = fract(52.9829189 * fract(dot(shadowTexel, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    return vec2(cos(angle), sin(angle));
}

float GE_SamplePointShadowSlot(int slot, vec3 lightPosition, vec3 position, vec3 normal, float filterRadius)
{
    // The bound SSBO range defines the available slots. An absent slot is lit.
    if (slot < 0 || slot >= int(ge_pointShadowSlots.length()))
        return 1.0;
    vec4 parameters = ge_pointShadowSlots[slot].params;
    vec4 tileParameters = ge_pointShadowSlots[slot].params2;
    if (parameters.x < 0.5)
        return 1.0; // slot disabled (defensive)

    float farPlane = tileParameters.z;
    vec3 lightToPosition = position - lightPosition;
    float distance = length(lightToPosition);
    if (distance <= 1e-6 || distance > farPlane)
        return 1.0;

    vec3 lightDirection = lightToPosition / distance;
    vec3 normalDirection = normalize(normal);
    float normalBias = parameters.w * (1.0 - abs(dot(normalDirection, lightDirection)));
    vec3 biasedPosition = position + normalDirection * normalBias;
    vec3 biasedLightToPosition = biasedPosition - lightPosition;
    float biasedDistance = length(biasedLightToPosition);
    if (biasedDistance <= 1e-6 || biasedDistance > farPlane)
        return 1.0;

    vec3 sampleDirection = biasedLightToPosition / biasedDistance;
    int face = GE_PointShadowFace(sampleDirection);
    mat4 faceVP = ge_pointShadowSlots[slot].faceVP[face];
    vec4 shadowClip = faceVP * vec4(biasedPosition, 1.0);
    if (shadowClip.w <= 1e-6)
        return 1.0;

    vec3 shadowNDC = shadowClip.xyz / shadowClip.w;
    vec2 faceUV = shadowNDC.xy * 0.5 + 0.5;
    faceUV.y = 1.0 - faceUV.y; // negative-viewport Y-flip, tile-local (tile at layer origin)
    float referenceDepth = shadowNDC.z + parameters.z;

    if (faceUV.x < 0.0 || faceUV.x > 1.0 ||
        faceUV.y < 0.0 || faceUV.y > 1.0 ||
        referenceDepth < 0.0 || referenceDepth > 1.0)
        return 1.0;

    float tileResolution = max(tileParameters.x, 1.0);
    vec2 rotation = GE_PointShadowRotation(faceUV, float(face), tileResolution);
    const int kTapCount = 12;
    float interiorMargin = abs(filterRadius) + 0.5 / tileResolution;
    if (all(greaterThanEqual(faceUV, vec2(interiorMargin))) &&
        all(lessThanEqual(faceUV, vec2(1.0 - interiorMargin))))
    {
        // A complete interior footprint keeps the original atlas arithmetic and
        // clamped hardware comparisons. Only boundary footprints need remapping.
        float tileScale = tileParameters.w;
        float atlasResolution = tileResolution / max(tileScale, 1e-6);
        float halfAtlasTexel = 0.5 / atlasResolution;
        vec2 subLo = vec2(halfAtlasTexel);
        vec2 subHi = vec2(tileScale - halfAtlasTexel);
        float layer = parameters.y + float(face);
        float shadow = 0.0;
        for (int tapIndex = 0; tapIndex < kTapCount; ++tapIndex)
        {
            vec2 offset = GE_VogelDisk(tapIndex, kTapCount, rotation) * filterRadius;
            vec2 atlasUV = clamp((faceUV + offset) * tileScale, subLo, subHi);
            shadow += GE_SHADOW_TAP(ge_pointShadowMap, vec4(atlasUV, layer, referenceDepth));
        }
        return shadow / float(kTapCount);
    }

    vec3 right = vec3(faceVP[0].x, faceVP[1].x, faceVP[2].x);
    vec3 up = vec3(faceVP[0].y, faceVP[1].y, faceVP[2].y);
    float shadow = 0.0;
    for (int tapIndex = 0; tapIndex < kTapCount; ++tapIndex)
    {
        vec2 offset = GE_VogelDisk(tapIndex, kTapCount, rotation) * filterRadius;
        vec3 tapOffset = (right * offset.x - up * offset.y) * (2.0 * shadowClip.w);
        shadow += GE_PointShadowTap(slot, face, biasedPosition + tapOffset,
            biasedLightToPosition + tapOffset, faceUV + offset, referenceDepth);
    }
    return shadow / float(kTapCount);
}
#endif
