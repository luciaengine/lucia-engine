// The normal that an impostor bake stores for a two-sided surface (adapter_forward.glsl, GE_IMPOSTOR_BAKE).
// Host-testable: no bindings, no other include.

#ifndef GE_IMPOSTOR_BAKE_NORMAL_GLSL
#define GE_IMPOSTOR_BAKE_NORMAL_GLSL

// A leaf has no front side: the mesh turns its normal toward the eye, and the eyes that see a card sit
// above its frame (the outer frames of the atlas look from the horizon). So the bake turns the normal
// toward the frame camera lifted toward +Y, and a leaf seen edge-on from a horizon frame keeps its upper
// side, which faces the sky and the sun. towardViewer: unit, surface to the frame camera, object space.
const float kImpostorTwoSidedLift = 0.45;
// The instance flag that marks a part drawn on both faces. MUST match GPUInstanceDepthClass.h
// kInstanceFlagDepthDoubleSided; the baker sets it from Material::IsDoubleSided.
const uint kImpostorBakeTwoSidedFlag = 8u;

vec3 GE_ImpostorTwoSidedBakeNormal(vec3 normal, vec3 towardViewer)
{
    vec3 facing = normalize(towardViewer + vec3(0.0, kImpostorTwoSidedLift, 0.0));
    return dot(normal, facing) < 0.0 ? -normal : normal;
}

#endif // GE_IMPOSTOR_BAKE_NORMAL_GLSL
