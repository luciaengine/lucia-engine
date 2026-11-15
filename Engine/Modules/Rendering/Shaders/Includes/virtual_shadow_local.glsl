#ifndef GE_VIRTUAL_SHADOW_LOCAL_GLSL
#define GE_VIRTUAL_SHADOW_LOCAL_GLSL

// The including clustered-lighting shader supplies the shared Vogel kernel,
// cube-face convention, rotation and world-independent local filter radius.
#include "virtual_shadow_projections.glsl"
#include "virtual_shadow_point.glsl"

bool GE_VirtualShadowLocalFilter(uint baseRow, int face, bool pointLight, vec3 biasedPosition,
                                 vec3 lightToReceiver, vec4 clip, vec2 centerUV,
                                 float reference, uint mip, out float visibility)
{
    const int taps = 12;
    GE_VirtualShadowProjection projection = ge_virtualShadowProjections[baseRow + uint(face)];
    float resolution = float(projection.Address.y * kGE_VirtualShadowPageTexels);
    vec2 rotation = GE_PointShadowRotation(centerUV, float(face), resolution);
    visibility = 0.0;
#ifndef GE_VIRTUAL_SHADOW_DISABLE_KERNEL_PAGE_REUSE
    GE_VirtualShadowKernelPage page;
    if (GE_VirtualShadowResolveKernelPage(projection.Address.xyz, mip, centerUV, kLocalFilterRadiusUV, reference, page))
    {
        for (int i = 0; i < taps; ++i)
        {
            vec2 uv = centerUV + GE_VogelDisk(i, taps, rotation) * kLocalFilterRadiusUV;
            visibility += GE_VirtualShadowCompareKernelPage(page, uv, reference);
        }
        visibility /= float(taps);
        return true;
    }
#endif
    for (int i = 0; i < taps; ++i)
    {
        vec2 offset = GE_VogelDisk(i, taps, rotation) * kLocalFilterRadiusUV;
        vec2 uv = centerUV + offset;
        float tap;
        if (pointLight)
        {
            mat4 vp = projection.RelativeToLightClip;
            vec3 right = vec3(vp[0].x, vp[1].x, vp[2].x);
            vec3 up = vec3(vp[0].y, vp[1].y, vp[2].y);
            vec3 directionOffset = (right * offset.x - up * offset.y) * (2.0 * clip.w);
            if (!GE_VirtualPointShadowTap(baseRow, face, mip, biasedPosition + directionOffset,
                lightToReceiver + directionOffset, uv, reference, tap))
                return false;
        }
        else if (!GE_VirtualShadowCompareBilinear(projection.Address.xyz, mip, uv, reference, tap))
            return false;
        visibility += tap;
    }
    visibility /= float(taps);
    return true;
}

bool GE_TryVirtualShadowLocal(bool pointLight, uint lightIndex, vec3 posRel, vec3 normal,
                              vec3 receiverDx, vec3 receiverDy, out float visibility)
{
    visibility = 1.0;
    if (lightIndex >= uint(ge_virtualShadowLocalRows.length()))
        return false;
    uint baseRow = ge_virtualShadowLocalRows[lightIndex];
    if (!GE_VirtualShadowHasProjection(baseRow) ||
        (pointLight && !GE_VirtualShadowHasProjection(baseRow + 5u)))
        return false;

    vec4 light = ge_virtualShadowProjections[baseRow].LightPositionRelAndFar;
    vec3 lightToReceiver = posRel - light.xyz;
    float distance = length(lightToReceiver);
    if (!(distance > 1e-6 && distance <= light.w))
        return false;

    vec3 n = normalize(normal);
    vec3 biasedPosition = posRel + n * (0.02 * (1.0 - abs(dot(n, lightToReceiver / distance))));
    vec3 biasedRay = biasedPosition - light.xyz;
    float biasedDistance = length(biasedRay);
    if (pointLight && !(biasedDistance > 1e-6 && biasedDistance <= light.w))
        return false;
    int face = pointLight ? GE_PointShadowFace(biasedRay) : 0;
    GE_VirtualShadowProjection projection = ge_virtualShadowProjections[baseRow + uint(face)];

    vec4 clip;
    vec2 uv;
    float reference;
    if (!GE_VirtualShadowProject(projection.RelativeToLightClip, biasedPosition, 0.0005, clip, uv, reference))
        return false;

    uint mip = GE_VirtualShadowReceiverMip(projection, clip, receiverDx, receiverDy);
    // Retry the entire kernel at each parent. A partly available fine
    // filter never contributes to a coarser result.
    for (uint level = mip; level <= projection.Address.z; ++level)
        if (GE_VirtualShadowLocalFilter(baseRow, face, pointLight, biasedPosition, biasedRay,
                                       clip, uv, reference, level, visibility))
            return true;
    return false;
}

#endif
