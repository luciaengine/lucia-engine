// Packed forest benchmark. The resident placement stream is shared with the card draw.
#define GE_FOREST_CARD_DRAW
#include "Includes/forest_placement.glsl"
#include "Includes/octahedral_normal.glsl"

layout(std430, set = 2, binding = 2) readonly buffer ForestVisiblePlacements
{
    ForestVisibleSplatTree visiblePlacements[];
};

layout(std140, set = 2, binding = 4) uniform ForestCardParameters
{
    vec4 grid; // Origin cell center XYZ, cell size.
    vec4 wind;
    uvec4 material;
} forestCard;

layout(std430, set = 2, binding = 5) readonly buffer ForestSplatCells
{
    uint splatWords[];
};

const vec2 kCorners[4] = vec2[4](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,1));

void ModifyVertex(inout VertexOutput vertex, inout InstanceData instance)
{
    uint index = uint(gl_VertexIndex);
    uint cell = (index >> 2u) * 3u;
    uint p = splatWords[cell];
    uint n = splatWords[cell + 1u];
    uint q = splatWords[cell + 2u];
    vec3 sourceCenter = forestCard.grid.xyz + forestCard.grid.w *
        vec3(p & 255u, (p >> 8u) & 255u, (p >> 16u) & 255u);
    vec3 center = sourceCenter * vec3(-1, 1, 1);
    vec3 normal = GE_OctDecode(vec2(n >> 24u, q & 255u) / 255.0) * vec3(-1, 1, 1);
    vec3 axis = abs(normal.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 right = normalize(cross(axis, normal));
    vec3 up = cross(normal, right);
    float coverage = float(p >> 24u) / 255.0;
    vec2 diskUV = kCorners[index & 3u];
    vec2 corner = diskUV * sqrt(coverage);

    ForestVisibleSplatTree row = visiblePlacements[gl_InstanceIndex];
    uvec2 packed = forestPlacements[row.placement];
    vec3 root = GE_ForestPlacementRoot(packed, forestPages[row.page]);
    float scale = GE_ForestPlacementScale(packed);
    float yaw = GE_ForestPlacementYaw(packed);
    float c = cos(yaw);
    float s = sin(yaw);
    mat3 rotation = mat3(vec3(c, 0, -s), vec3(0, 1, 0), vec3(s, 0, c));

    instance.materialIndex = forestCard.material.x;
    instance.flags = 2u;
    instance.sector = Cam.uRenderOriginSector.xyz;

    vec3 offset = (right * corner.x + up * corner.y) * (forestCard.grid.w * 0.79788456);
    float bend = float((q >> 8u) & 255u) / 255.0;
    vec3 sway = vec3(row.sway.x, 0, row.sway.y) * bend;
    vertex.position = root + (rotation * (center + offset) + sway) * scale;
    vertex.sector = instance.sector;
    vertex.useSector = true;
    vertex.normal = rotation * normal;
    vertex.uv0 = diskUV;
    float leafFraction = float((q >> 16u) & 255u) / 255.0;
    vertex.custom0 = vec4(vec3(n & 255u, (n >> 8u) & 255u, (n >> 16u) & 255u) / 255.0, leafFraction);
}
