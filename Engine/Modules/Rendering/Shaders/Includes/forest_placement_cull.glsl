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

shared uint pageVisibleCount;
shared uint pageOutputStart;
shared uint pagePlacements[kForestPlacementsPerSlot];

void main()
{
    const uint pageIndex = gl_WorkGroupID.x;
    const uint lane = gl_LocalInvocationID.x;
    const ForestPlacementPage page = forestPages[pageIndex];

    const uint count = min(page.range.y, kForestPlacementsPerSlot);
    const bool inside = page.range.z != 0u;
    const bool entirePage = inside && all(equal(page.meshOwnedLow | page.meshOwnedHigh, uvec4(0u)));
    if (lane == 0u)
        pageVisibleCount = entirePage ? count : 0u;
    barrier();

    if (!entirePage)
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
