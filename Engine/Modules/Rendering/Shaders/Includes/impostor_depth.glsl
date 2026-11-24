// The generated card streams use UV0. Locations 8..10 carry the object axes in
// render-space metres per sphere radius. Separate vectors also fit the compatibility profile.
#ifdef GE_STAGE_VERTEX
flat layout(location = 8) out vec3 vCardAxisX;
flat layout(location = 9) out vec3 vCardAxisY;
flat layout(location = 10) out vec3 vCardAxisZ;

void GE_SetImpostorDepthFrame(mat3 linear, float radius)
{
    vCardAxisX = linear[0] * radius;
    vCardAxisY = linear[1] * radius;
    vCardAxisZ = linear[2] * radius;
}
#else
flat layout(location = 8) in vec3 vCardAxisX;
flat layout(location = 9) in vec3 vCardAxisY;
flat layout(location = 10) in vec3 vCardAxisZ;

#ifndef GE_CAMERA_RELATIVE_GLSL
layout(set = 0, binding = 5) uniform CameraUBO {
#include "camera_ubo_fields.glsl"
} Cam;
#include "camera_relative.glsl"
#endif

float GE_ImpostorAttachmentDepth(vec3 cardPositionRel, vec3 offset)
{
    vec3 hitRel = cardPositionRel + mat3(vCardAxisX, vCardAxisY, vCardAxisZ) * offset;
    vec4 clip = GE_ProjectDepthPositionRel(hitRel);
    if (clip.w <= 0.0 || clip.z < 0.0 || clip.z > clip.w)
        discard;
    return clip.z / clip.w;
}
#endif
