#define GE_FOREST_CARD_DRAW
#include "Includes/forest_placement.glsl"
#include "Includes/impostor_frame.glsl"
#include "Includes/impostor_depth.glsl"
#include "Includes/foliage_sway.glsl"

layout(std430, set = 2, binding = 2) readonly buffer ForestVisiblePlacements
{
    ForestVisiblePlacement visiblePlacements[];
};

layout(std140, set = 2, binding = 4) uniform ForestCardParameters
{
    vec4 atlas;
    vec4 wind;
    uvec4 material;
    // What the card faces, relative to the render origin: the eye point (w = 1) in a color pass,
    // the unit direction toward the light (w = 0) in a sun cascade.
    vec4 viewer;
} forestCard;

const vec2 kForestCardUv[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1),
                                     vec2(0, 1), vec2(1, 0), vec2(1, 1));

void ModifyVertex(inout VertexOutput outputVertex, inout InstanceData instance)
{
    const ForestVisiblePlacement row = visiblePlacements[gl_InstanceIndex];
    const uvec2 packed = forestPlacements[row.placement];
    const vec3 root = GE_ForestPlacementRoot(packed, forestPages[row.page]);
    const float scale = GE_ForestPlacementScale(packed);
    const float yaw = GE_ForestPlacementYaw(packed);
    const float c = cos(yaw);
    const float s = sin(yaw);
    const mat3 rotation = mat3(vec3(c, 0, -s), vec3(0, 1, 0), vec3(s, 0, c));

    const vec4 bounds = forestView.atlasBounds;
    const vec3 toViewer = GE_ImpostorTowardViewer(forestCard.viewer, root + rotation * bounds.xyz * scale);
    const float viewerDistance = length(toViewer);
    const vec3 localViewer = viewerDistance > 1e-6 ? transpose(rotation) * (toViewer / viewerDistance) : vec3(0, 1, 0);
    const vec2 code = forestCard.atlas.y < 0.5 ? GE_HemiOctEncode(localViewer) : GE_OctEncode(localViewer);
    const vec3 normal = GE_ImpostorDecode(code, forestCard.atlas);
    vec3 right;
    vec3 up;
    GE_ImpostorFrameAxes(normal, right, up);
    const vec2 uv = kForestCardUv[gl_VertexIndex];
    const vec2 corner = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec3 localVertex = bounds.xyz + (right * corner.x + up * corner.y) * bounds.w;

    instance.materialIndex = forestCard.material.x;
    instance.sector = Cam.uRenderOriginSector.xyz;
    instance.modelMatrix = mat4(vec4(rotation[0] * scale, 0), vec4(rotation[1] * scale, 0),
                                vec4(rotation[2] * scale, 0), vec4(root, 1));
    GE_SetImpostorDepthFrame(mat3(instance.modelMatrix), bounds.w);
    localVertex = GE_ApplyFoliageSway(localVertex, instance, forestCard.wind.xy, forestCard.wind.z);
    outputVertex.position = root + rotation * localVertex * scale;
    outputVertex.sector = instance.sector;
    outputVertex.useSector = true;
    outputVertex.normal = rotation * normal;
    outputVertex.uv0 = uv;
    outputVertex.custom0 = vec4(code, c, s);
}
