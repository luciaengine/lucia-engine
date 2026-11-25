#ifndef GE_FOREST_PLACEMENT_GLSL
#define GE_FOREST_PLACEMENT_GLSL

const uint kForestPlacementsPerSlot = 144u;

struct ForestPlacementPage
{
    uvec4 range; // First placement slot, count, roots fully inside the frustum, handoff fade code.
    vec4 originRel;
    uvec4 meshOwnedLow;
    uvec4 meshOwnedHigh;
    vec4 boundsMinRel;
    vec4 boundsMaxRel;
};

struct ForestVisiblePlacement
{
    uint placement;
    uint page;
};

// A visible tree of a splat draw, with its wind sway in meters at full bend.
struct ForestVisibleSplat
{
    uint placement;
    uint page;
    vec2 sway;
};

#ifdef GE_FOREST_CARD_DRAW
#define GE_FOREST_SLOT_SET 2
#define GE_FOREST_VIEW_SET 2
#define GE_FOREST_VIEW_BINDING 3
#else
#define GE_FOREST_SLOT_SET 0
#define GE_FOREST_VIEW_SET 1
#define GE_FOREST_VIEW_BINDING 0
#endif

layout(std430, set = GE_FOREST_SLOT_SET, binding = 0) readonly buffer ForestPlacementSlots
{
    uvec2 forestPlacements[];
};
layout(std430, set = GE_FOREST_SLOT_SET, binding = 1) readonly buffer ForestPlacementPages
{
    ForestPlacementPage forestPages[];
};
layout(std140, set = GE_FOREST_VIEW_SET, binding = GE_FOREST_VIEW_BINDING) uniform ForestPlacementView
{
    mat4 viewProjectionRel;
    vec4 planes[6];
    vec4 eyeRelAndRadius;
    vec4 atlasBounds;
} forestView;

#undef GE_FOREST_SLOT_SET
#undef GE_FOREST_VIEW_SET
#undef GE_FOREST_VIEW_BINDING

bool GE_ForestPlacementMeshOwned(ForestPlacementPage page, uint localPlacement)
{
    const uint word = localPlacement >> 5u;
    const uint mask = word < 4u ? page.meshOwnedLow[word & 3u] : page.meshOwnedHigh[word & 3u];
    return (mask & (1u << (localPlacement & 31u))) != 0u;
}

// A placement is PageStreaming::PackedPlacement: x X and Z in centimeters, y the height in
// centimeters, then the yaw in 1/256 turns and the scale in 1/255 steps from 0.75 to 1.25.
vec3 GE_ForestPlacementRoot(uvec2 packed, ForestPlacementPage page)
{
    const vec3 centimeters = vec3(packed.x & 0xffffu, packed.y & 0xffffu, packed.x >> 16u);
    return page.originRel.xyz + centimeters * 0.01;
}

float GE_ForestPlacementScale(uvec2 packed)
{
    return 0.75 + float(packed.y >> 24u) * (0.5 / 255.0);
}

float GE_ForestPlacementYaw(uvec2 packed)
{
    return float((packed.y >> 16u) & 0xffu) * (6.283185307179586 / 256.0);
}

#endif
