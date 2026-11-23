// Animated splats of a packed forest stand: four vertices per baked cell, one instance per visible tree.
// The placement stream is the one the impostor cards read; forest_splat_cull.comp writes the rows.
#define GE_FOREST_CARD_DRAW
#include "Includes/forest_placement.glsl"
#include "Includes/forest_splat_disk.glsl"
#include "Includes/octahedral_normal.glsl"

layout(std430, set = 2, binding = 2) readonly buffer ForestVisiblePlacements
{
    ForestVisibleSplat visiblePlacements[];
};

layout(std140, set = 2, binding = 4) uniform ForestCardParameters
{
    vec4 grid; // Center of cell (0, 0, 0) in model space, cell size in meters.
    vec4 wind;
    uvec4 material;
} forestCard;

// ForestSplatCell (ForestSplatCells.h): three words per cell.
layout(std430, set = 2, binding = 5) readonly buffer ForestSplatCells
{
    uint splatWords[];
};

const vec2 kForestSplatCorners[4] = vec2[4](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));
// The disk radius in cells at full coverage: sqrt(2 / pi), a disk with the area of two cell faces.
const float kForestSplatDiskRadius = 0.79788456;

void ModifyVertex(inout VertexOutput outputVertex, inout InstanceData instance)
{
    const uint vertexIndex = uint(gl_VertexIndex);
    const uint cell = (vertexIndex >> 2u) * 3u;
    const uint positionCoverage = splatWords[cell];
    const uint colorNormalX = splatWords[cell + 1u];
    const uint normalYBendLeaf = splatWords[cell + 2u];
    const vec3 center = forestCard.grid.xyz + forestCard.grid.w *
        vec3(positionCoverage & 255u, (positionCoverage >> 8u) & 255u, (positionCoverage >> 16u) & 255u);
    const vec3 normal = GE_OctDecode(vec2(colorNormalX >> 24u, normalYBendLeaf & 255u) / 255.0);
    const float coverage = float(positionCoverage >> 24u) / 255.0;
    const float bend = float((normalYBendLeaf >> 8u) & 255u) / 255.0;
    const float leafFraction = float((normalYBendLeaf >> 16u) & 255u) / 255.0;
    const vec2 diskUv = kForestSplatCorners[vertexIndex & 3u];

    const ForestVisibleSplat row = visiblePlacements[gl_InstanceIndex];
    const uvec2 packed = forestPlacements[row.placement];
    const vec3 root = GE_ForestPlacementRoot(packed, forestPages[row.page]);
    const float scale = GE_ForestPlacementScale(packed);
    const float yaw = GE_ForestPlacementYaw(packed);
    const float c = cos(yaw);
    const float s = sin(yaw);
    const mat3 rotation = mat3(vec3(c, 0, -s), vec3(0, 1, 0), vec3(s, 0, c));

#ifdef GE_LOD_CROSSFADE
    instance.lodFadeCode = forestPages[row.page].range.w;
#endif
    instance.materialIndex = forestCard.material.x;
    instance.sector = Cam.uRenderOriginSector.xyz;

    const vec3 viewAxis = vec3(Cam.uVRel[0].z, Cam.uVRel[1].z, Cam.uVRel[2].z);
    const GE_ForestSplatDisk disk = GE_ForestSplatDiskFor(normal, transpose(rotation) * viewAxis, bend, leafFraction);
    const vec3 axis = abs(disk.normal.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    const vec3 right = normalize(cross(axis, disk.normal));
    const vec3 up = cross(disk.normal, right);
    const vec2 corner = diskUv * sqrt(coverage) * disk.growth;
    const vec3 offset = (right * corner.x + up * corner.y) * (forestCard.grid.w * kForestSplatDiskRadius);
    const vec3 sway = vec3(row.sway.x, 0, row.sway.y) * bend;
    outputVertex.position = root + (rotation * (center + offset) + sway) * scale;
    outputVertex.sector = instance.sector;
    outputVertex.useSector = true;
    outputVertex.normal = rotation * normal;
    outputVertex.uv0 = diskUv;
    outputVertex.custom0 = vec4(vec3(colorNormalX & 255u, (colorNormalX >> 8u) & 255u,
                                     (colorNormalX >> 16u) & 255u) / 255.0, leafFraction);
}
