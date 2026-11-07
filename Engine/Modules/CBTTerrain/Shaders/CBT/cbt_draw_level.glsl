// Terrain draw levels mirror CBTDrawLevelParams in this frame's set-2 ring slot.
// Vertices use distance-filtered heights; fragments use distance-filtered normals.
// The compatibility profile has no chain.
#ifndef CBT_DRAW_LEVEL_GLSL
#define CBT_DRAW_LEVEL_GLSL

struct CBTDrawLevelParamsData
{
    vec4 eye;     // xyz = the CBT camera (world), w = the level scale; 0 = keep the cached corners
    vec4 height;  // x = height scale, y = origin Y
    uvec4 lattice; // x = the level-0 samples (x | z << 16) the chain must have been built from;
                   // y = the normal chain's mips, 0 = the surface keeps the level-0 normal map
};
layout(std140, set = 2, binding = 14) uniform CBTDrawLevelBuffer
{
    CBTDrawLevelParamsData Params;
} gCbtDrawLevel;

#endif // CBT_DRAW_LEVEL_GLSL
