#ifndef GE_VIRTUAL_SHADOW_DIRECTIONAL_GLSL
#define GE_VIRTUAL_SHADOW_DIRECTIONAL_GLSL

#include "virtual_shadow_projections.glsl"

// Virtual windows are independent of conventional CSM depth splits. Resolve
// the first complete footprint once, before any conventional cascade blend.
// Other authored filters retain the conventional path until implemented here.
bool GE_TryVirtualShadowDirectional(vec3 position, vec3 normal, float cosTheta, vec2 screenPos,
                                     int quality, out float visibility)
{
    visibility = 1.0;
    if (quality != 2)
        return false;
    float refDepthSpan = max(ge_shadowPcssCascades[0].y, 1e-3);
    float refWorldPerTexel = max(ge_shadowPcssCascades[0].x, 1e-6);
    for (int level = 0; level < min(int(ge_shadowParams.z), 4); ++level)
    {
        uint row = ge_virtualShadowDirectionalRows[level];
        if (!GE_VirtualShadowHasProjection(row))
            continue;
        GE_VirtualShadowProjection projection = ge_virtualShadowProjections[row];
        float worldPerTexel = max(projection.LightPositionRelAndFar.x, 1e-6);
        float normalBias = GE_ShadowNormalBiasWorld(ge_shadowParams.y, worldPerTexel,
            refWorldPerTexel, quality, ge_shadowPcss.x, cosTheta, 0.0, false, 0.0);
        vec4 clip;
        vec2 uv;
        float reference;
        float depthSpan = max(projection.LightPositionRelAndFar.y, 1e-3);
        if (!GE_VirtualShadowProject(projection.RelativeToLightClip, position + normal * normalBias,
                                     ge_shadowParams.x * refDepthSpan / depthSpan, clip, uv, reference))
            continue;
        float resolution = float(projection.Address.y * kGE_VirtualShadowPageTexels);
        vec2 rotation = GE_PoissonRotation(screenPos, uv, vec2(resolution));
        float radius = (1.5 / resolution) * max(ge_shadowPcss.x, 0.0) * (refWorldPerTexel / worldPerTexel);
        int taps = GE_PcssTapCount();
#ifndef GE_VIRTUAL_SHADOW_DISABLE_KERNEL_PAGE_REUSE
        GE_VirtualShadowKernelPage page;
        if (GE_VirtualShadowResolveKernelPage(projection.Address.xyz, 0u, uv, radius, reference, page))
        {
            float sum = 0.0;
            for (int i = 0; i < taps; ++i)
                sum += GE_VirtualShadowCompareKernelPage(page, uv + GE_VogelDisk(i, taps, rotation) * radius, reference);
            visibility = sum / float(taps);
            return true;
        }
#endif
        GE_VirtualShadowKernelQuad quad;
        if (GE_VirtualShadowResolveKernelQuad(projection.Address.xyz, 0u, uv, radius, quad))
        {
            float sum = 0.0;
            for (int i = 0; i < taps; ++i)
                sum += GE_VirtualShadowCompareKernelQuad(quad, uv + GE_VogelDisk(i, taps, rotation) * radius, reference);
            visibility = sum / float(taps);
            return true;
        }
    }
    return false;
}

#endif
