#ifndef GE_VIRTUAL_SHADOW_DIRECTIONAL_GLSL
#define GE_VIRTUAL_SHADOW_DIRECTIONAL_GLSL

#include "virtual_shadow_projections.glsl"
#include "shadow_filter.glsl"
#include "shadow_pcss_decisions.glsl"
#include "shadow_pcss_taps.glsl"

// The ShadowFilterQuality values that the virtual sun pages filter. The others keep the cascades.
const int kGE_VirtualShadowPoisson = 2;
const int kGE_VirtualShadowPcss = 3;

// The average raw depth of the search taps nearer the light than the receiver; -1 when none.
// Reverse-Z: nearer the light is larger. The taps must lie inside the resolved quad.
float GE_VirtualShadowBlockerSearch(GE_VirtualShadowKernelQuad quad, vec2 uv, float reference, vec2 rotation,
                                    float radiusUV, int taps)
{
    float sum = 0.0;
    int count = 0;
    for (int i = 0; i < kPcssMaxTaps; ++i)
    {
        if (i >= taps)
            break;
        float depth = GE_VirtualShadowQuadDepth(quad, uv + GE_VogelDisk(i, taps, rotation) * radiusUV);
        if (GE_PcssIsBlocker(depth, reference, vec2(0.0), vec2(0.0)))
        {
            sum += depth;
            ++count;
        }
    }
    return count > 0 ? sum / float(count) : -1.0;
}

// PCSS on one resident level, with the cascade filter's search and penumbra decisions
// (shadow_pcss_decisions.glsl). The pages have no receiver-plane gradient.
bool GE_VirtualShadowPcss(uvec3 address, vec2 uv, float reference, vec2 rotation, float resolution,
                          float worldPerTexel, float depthSpan, float tanHalfAngle, float capWorld,
                          out float visibility)
{
    visibility = 1.0;
    float capTexels = max(capWorld, worldPerTexel) / worldPerTexel;
    float searchWorld = tanHalfAngle * depthSpan * kPcssSearchDepthFraction;
    int searchTaps = GE_PcssTapsForRadius(searchWorld / worldPerTexel);
    float searchTexels = clamp(searchWorld / worldPerTexel, kPcssMinSearchTexels,
                               kPcssSearchTexelsPerRootTap * sqrt(float(searchTaps)));
    searchTexels = GE_PcssBoundedSearchTexels(searchTexels, capTexels, kPcssMinSearchTexels);
    searchTaps = GE_PcssTapsForRadius(searchTexels);
    GE_VirtualShadowKernelQuad quad;
    if (!GE_VirtualShadowResolveKernelQuad(address, 0u, uv, searchTexels / resolution, quad))
        return false;
    float blocker = GE_VirtualShadowBlockerSearch(quad, uv, reference, rotation, searchTexels / resolution, searchTaps);
    float penumbraTexels = GE_PcssPenumbraTexels(blocker, reference, depthSpan, tanHalfAngle, worldPerTexel, capWorld);
    // A kernel wider than the search needs its own resident pages.
    if (penumbraTexels > searchTexels &&
        !GE_VirtualShadowResolveKernelQuad(address, 0u, uv, penumbraTexels / resolution, quad))
        return false;
    int taps = GE_PcssTapsForRadius(penumbraTexels);
    float radius = penumbraTexels / resolution;
    float sum = 0.0;
    for (int i = 0; i < kPcssMaxTaps; ++i)
    {
        if (i >= taps)
            break;
        sum += GE_VirtualShadowCompareKernelQuad(quad, uv + GE_VogelDisk(i, taps, rotation) * radius, reference);
    }
    visibility = sum / float(taps);
    return true;
}

// Virtual windows are independent of conventional CSM depth splits. Resolve
// the first complete footprint once, before any conventional cascade blend.
bool GE_TryVirtualShadowDirectional(vec3 position, vec3 normal, float cosTheta, vec2 screenPos,
                                     int quality, out float visibility)
{
    visibility = 1.0;
    if (quality != kGE_VirtualShadowPoisson && quality != kGE_VirtualShadowPcss)
        return false;
    float refDepthSpan = max(ge_shadowPcssCascades[0].y, 1e-3);
    float refWorldPerTexel = max(ge_shadowPcssCascades[0].x, 1e-6);
    // tan(half angular diameter) of the sun, the same on every cascade.
    float tanHalfAngle = max(ge_shadowPcssCascades[0].w, 0.0);
    float capWorld = ge_shadowPcss.z > 0.0 ? ge_shadowPcss.z : kPcssMaxPenumbraWorldAuto;
    for (int level = 0; level < min(int(ge_shadowParams.z), 4); ++level)
    {
        uint row = ge_virtualShadowDirectionalRows[level];
        if (!GE_VirtualShadowHasProjection(row))
            continue;
        GE_VirtualShadowProjection projection = ge_virtualShadowProjections[row];
        float worldPerTexel = max(projection.LightPositionRelAndFar.x, 1e-6);
        float normalBias = GE_ShadowNormalBiasWorld(ge_shadowParams.y, worldPerTexel,
            refWorldPerTexel, quality, ge_shadowPcss.x, cosTheta, capWorld, false, 0.0);
        vec4 clip;
        vec2 uv;
        float reference;
        float depthSpan = max(projection.LightPositionRelAndFar.y, 1e-3);
        if (!GE_VirtualShadowProject(projection.RelativeToLightClip, position + normal * normalBias,
                                     ge_shadowParams.x * refDepthSpan / depthSpan, clip, uv, reference))
            continue;
        float resolution = float(projection.Address.y * kGE_VirtualShadowPageTexels);
        vec2 rotation = GE_PoissonRotation(screenPos, uv, vec2(resolution));
        if (quality == kGE_VirtualShadowPcss)
        {
            if (GE_VirtualShadowPcss(projection.Address.xyz, uv, reference, rotation, resolution, worldPerTexel,
                                     depthSpan, tanHalfAngle, capWorld, visibility))
                return true;
            continue;
        }
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
