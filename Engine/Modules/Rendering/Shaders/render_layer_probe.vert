#version 450

// Test raster consumer of the production cull -> scatter indirection. No
// secondary mask test here: a leaked instance must reach the readback image.
struct GPUInstance {
#include "Includes/gpu_instance_fields.glsl"
};
layout(std430, set = 0, binding = 0) readonly buffer Instances { GPUInstance instances[]; };
layout(std430, set = 0, binding = 1) readonly buffer Indirection { uint indices[]; };
layout(location = 0) flat out vec4 instancePayload;
void main() {
    const vec2 corners[3] = vec2[3](vec2(-.12,-.2), vec2(.12,-.2), vec2(0,.2));
    GPUInstance instance = instances[indices[gl_InstanceIndex]];
    gl_Position = instance.transform * vec4(corners[gl_VertexIndex] * instance.custom0.w, .5, 1);
    instancePayload = vec4(instance.custom0.xy, float((instance.materialIndex & 7u) + 1u) / 8.0, 1);
}
