#include "Includes/forest_placement.glsl"
#include "Includes/frustum_tests.glsl"
#include "Includes/storage_access.glsl"

// Compacts the visible placements of the resident pages into the indirect draw of one stand.
// With GE_FOREST_SPLAT_CULL each row also holds the tree's wind sway, resolved once per tree.
#ifdef GE_FOREST_SPLAT_CULL
#include "Includes/instance_sector.glsl"

layout(std140, set = 1, binding = 1) uniform ForestWindParameters
{
    ivec4 uSector;
    vec4 uTimeAndDisplacement; // Deformation clock in seconds, largest sway in meters.
#include "Includes/weather_wind_fields.glsl"
    vec4 uScreenFootprint; // Fine cell width, pixels per world unit, footprint threshold, orthographic flag.
} forestWind;

#define GE_WEATHER_LIGHT forestWind
#include "Includes/weather_wind_resolve.glsl"

// The horizontal sway in meters of a tree's crown at full bend, from the weather wind at its root.
vec2 GE_VisibleTreeSway(uint slot, ForestPlacementPage page)
{
    vec3 root = GE_ForestPlacementRoot(forestPlacements[slot], page) +
                vec3(forestWind.uSector.xyz) * GE_SECTOR_SIZE;
    GE_ResolvedWind wind = GE_ResolveWeatherWind(root, GE_DefaultWind());
    float speed = length(wind.velocity.xz);
    if (speed < 0.0001)
        return vec2(0);
    float phase = dot(root.xz, vec2(0.6, 0.8)) / wind.gustScale;
    float gust = sin(6.2831853 * (forestWind.uTimeAndDisplacement.x * wind.frequency + phase));
    return wind.velocity.xz * (gust * forestWind.uTimeAndDisplacement.y * min(speed * 0.1, 1.0) / speed);
}
#endif

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 2) GE_STORAGE_WRITEONLY buffer ForestVisiblePlacements
{
#ifdef GE_FOREST_SPLAT_CULL
    ForestVisibleSplat visiblePlacements[];
#else
    ForestVisiblePlacement visiblePlacements[];
#endif
};

layout(std430, set = 0, binding = 3) buffer ForestPlacementIndirectDraw
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
} indirectDraw;

#ifdef GE_FOREST_SPLAT_CULL
layout(std430, set = 0, binding = 4) GE_STORAGE_WRITEONLY buffer ForestCoarsePlacements
{
    ForestVisibleSplat coarsePlacements[];
};
layout(std430, set = 0, binding = 5) buffer ForestCoarseIndirectDraw
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
} coarseDraw;

shared uint fineCount;
shared uint coarseCount;
shared uint fineStart;
shared uint coarseStart;
shared uint fineSlots[kForestPlacementsPerSlot];
shared uint coarseSlots[kForestPlacementsPerSlot];

bool WantsCoarseSplats(uint slot, ForestPlacementPage page)
{
    const uvec2 packed = forestPlacements[slot];
    const float scale = GE_ForestPlacementScale(packed);
    const vec3 root = GE_ForestPlacementRoot(packed, page);
    const float clipW = (forestView.viewProjectionRel * vec4(root, 1.0)).w;
    const float nearDistance = forestWind.uScreenFootprint.w > 0.0
        ? 1.0 : clipW - scale * forestView.eyeRelAndRadius.w;
    const float pixelsAtUnitDistance = forestWind.uScreenFootprint.x * scale * forestWind.uScreenFootprint.y;
    return nearDistance > 0.0 && pixelsAtUnitDistance <= forestWind.uScreenFootprint.z * nearDistance;
}
#endif

shared uint pageVisibleCount;
shared uint pageOutputStart;
shared uint pagePlacements[kForestPlacementsPerSlot];

#ifdef GE_FOREST_PAGE_OCCLUSION
layout(set = 2, binding = 0) uniform sampler2D forestOcclusionHZB;
shared bool pageOccluded;

bool ForestPageOccluded(ForestPlacementPage page)
{
    vec2 ndcMin = vec2(1.0);
    vec2 ndcMax = vec2(-1.0);
    float nearestDepth = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        const bvec3 high = notEqual(uvec3(corner) & uvec3(1u, 2u, 4u), uvec3(0u));
        const vec3 point = mix(page.boundsMinRel.xyz, page.boundsMaxRel.xyz, high);
        const vec4 clip = forestView.viewProjectionRel * vec4(point, 1.0);
        // Clipped edges can be visible even when none of their corners are.
        if (!(clip.w > 0.0 && clip.z >= 0.0 && clip.z <= clip.w) || any(isinf(clip)))
            return false;
        const vec3 ndc = clip.xyz / clip.w;
        if (any(isinf(ndc)) || any(isnan(ndc)))
            return false;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        nearestDepth = max(nearestDepth, ndc.z);
    }

    const vec2 uvMin = clamp(vec2(ndcMin.x, -ndcMax.y) * 0.5 + 0.5, 0.0, 1.0);
    const vec2 uvMax = clamp(vec2(ndcMax.x, -ndcMin.y) * 0.5 + 0.5, 0.0, 1.0);
    const ivec2 size = textureSize(forestOcclusionHZB, 0);
    // Include a pixel on each edge for rasterization and projection rounding.
    const ivec2 first = clamp(ivec2(floor(uvMin * vec2(size))) - 1, ivec2(0), size - 1);
    const ivec2 last = clamp(ivec2(floor(uvMax * vec2(size))) + 1, ivec2(0), size - 1);
    const int span = max(last.x - first.x, last.y - first.y) + 1;
    const int mip = min(findMSB(span - 1) + 1, textureQueryLevels(forestOcclusionHZB) - 1);
    const ivec2 mipMax = textureSize(forestOcclusionHZB, mip) - 1;
    const ivec2 low = min(first >> mip, mipMax);
    const ivec2 high = min(last >> mip, mipMax);
    if (any(greaterThan(high - low, ivec2(1))))
        return false;

    float occluder = texelFetch(forestOcclusionHZB, low, mip).r;
    occluder = min(occluder, texelFetch(forestOcclusionHZB, ivec2(low.x, high.y), mip).r);
    occluder = min(occluder, texelFetch(forestOcclusionHZB, ivec2(high.x, low.y), mip).r);
    occluder = min(occluder, texelFetch(forestOcclusionHZB, high, mip).r);
    return nearestDepth + 1e-5 < occluder;
}
#endif

void main()
{
    const uint pageIndex = gl_WorkGroupID.x;
    const uint lane = gl_LocalInvocationID.x;
    const ForestPlacementPage page = forestPages[pageIndex];

    const uint count = min(page.range.y, kForestPlacementsPerSlot);
    const bool inside = page.range.z != 0u;
    const bool entirePage = inside && all(equal(page.meshOwnedLow | page.meshOwnedHigh, uvec4(0u)));
    if (lane == 0u)
    {
    #ifdef GE_FOREST_PAGE_OCCLUSION
        pageOccluded = ForestPageOccluded(page);
        pageVisibleCount = !pageOccluded && entirePage ? count : 0u;
    #else
        pageVisibleCount = entirePage ? count : 0u;
    #endif
    }
    barrier();

    if (!entirePage
#ifdef GE_FOREST_PAGE_OCCLUSION
        && !pageOccluded
#endif
        )
    {
        for (uint i = lane; i < count; i += gl_WorkGroupSize.x)
        {
            if (GE_ForestPlacementMeshOwned(page, i))
                continue;
            const uint slot = page.range.x + i;
            if (!inside)
            {
                const uvec2 packed = forestPlacements[slot];
                const vec3 root = GE_ForestPlacementRoot(packed, page);
                const float radius = GE_ForestPlacementScale(packed) * forestView.eyeRelAndRadius.w;
                if (SphereOutsideFrustum(root, radius, forestView.planes))
                    continue;
            }
            pagePlacements[atomicAdd(pageVisibleCount, 1u)] = slot;
        }
    }
    barrier();

#ifdef GE_FOREST_SPLAT_CULL
    if (forestWind.uScreenFootprint.x > 0.0)
    {
        if (lane == 0u)
        {
            fineCount = 0u;
            coarseCount = 0u;
        }
        barrier();
        for (uint i = lane; i < pageVisibleCount; i += gl_WorkGroupSize.x)
        {
            const uint slot = entirePage ? page.range.x + i : pagePlacements[i];
            if (WantsCoarseSplats(slot, page))
                coarseSlots[atomicAdd(coarseCount, 1u)] = slot;
            else
                fineSlots[atomicAdd(fineCount, 1u)] = slot;
        }
        barrier();
        if (lane == 0u)
        {
            if (fineCount != 0u)
                fineStart = atomicAdd(indirectDraw.instanceCount, fineCount);
            if (coarseCount != 0u)
                coarseStart = atomicAdd(coarseDraw.instanceCount, coarseCount);
        }
        barrier();
        for (uint i = lane; i < fineCount; i += gl_WorkGroupSize.x)
        {
            const uint slot = fineSlots[i];
            visiblePlacements[fineStart + i] = ForestVisibleSplat(slot, pageIndex, GE_VisibleTreeSway(slot, page));
        }
        for (uint i = lane; i < coarseCount; i += gl_WorkGroupSize.x)
        {
            const uint slot = coarseSlots[i];
            coarsePlacements[coarseStart + i] = ForestVisibleSplat(slot, pageIndex, GE_VisibleTreeSway(slot, page));
        }
        return;
    }
#endif

    // The writer reserves one tail range per page. The output has space
    // for every placement in the unique resident page list.
    if (lane == 0u && pageVisibleCount != 0u)
        pageOutputStart = atomicAdd(indirectDraw.instanceCount, pageVisibleCount);
    barrier();

    for (uint i = lane; i < pageVisibleCount; i += gl_WorkGroupSize.x)
    {
        const uint slot = entirePage ? page.range.x + i : pagePlacements[i];
#ifdef GE_FOREST_SPLAT_CULL
        visiblePlacements[pageOutputStart + i] = ForestVisibleSplat(slot, pageIndex, GE_VisibleTreeSway(slot, page));
#else
        visiblePlacements[pageOutputStart + i] = ForestVisiblePlacement(slot, pageIndex);
#endif
    }
}
