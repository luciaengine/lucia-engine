// Impostor card vertex modifier: places the four corners of a quad (UV0 in [0,1]) on the plane
// that faces the instance's view direction, centred on the impostor sphere and as wide as it.
// Object space; the adapter applies the instance transform.
//
// @property vec4 impostorBounds "Impostor Bounds" default=0,0,0,1 group=Impostor

#include "Includes/impostor_frame.glsl"

vec3 ModifyVertex(vec3 position, InstanceData inst)
{
    vec3 right;
    vec3 up;
    GE_ImpostorFrameAxes(GE_ImpostorDecode(inst.custom0.xy, Props.impostorAtlas), right, up);
#ifdef HAS_UV0
    // UV (0,0) is the card's top-left, as in a baked tile.
    vec2 corner = vec2(aUV0.x * 2.0 - 1.0, 1.0 - aUV0.y * 2.0);
#else
    vec2 corner = vec2(0.0);
#endif
    return Props.impostorBounds.xyz + (right * corner.x + up * corner.y) * Props.impostorBounds.w;
}
