// The impostor frame an instance shows (Engine/Rendering/Impostors). custom0.xy of the instance
// is the octahedral code of its view direction (from the object toward the camera, object space);
// the frame is the atlas tile that code falls in. The axes follow MakeImpostorFrameAxes
// (ImpostorFrameCamera.cpp), so a card spans exactly the image its frame camera baked.

#ifndef GE_IMPOSTOR_FRAME_GLSL
#define GE_IMPOSTOR_FRAME_GLSL
#include "octahedral_normal.glsl"

// impostorAtlas: x = frames per side, y = layout (0 hemi-octahedral around +Y, 1 octahedral).
vec2 GE_ImpostorFrame(vec2 viewCode, vec4 impostorAtlas)
{
    float frames = max(impostorAtlas.x, 1.0);
    return clamp(floor(viewCode * frames), vec2(0.0), vec2(frames - 1.0));
}

vec3 GE_ImpostorFrameDirection(vec2 frame, vec4 impostorAtlas)
{
    vec2 center = (frame + 0.5) / max(impostorAtlas.x, 1.0);
    return impostorAtlas.y < 0.5 ? GE_HemiOctDecode(center) : GE_OctDecode(center);
}

// Right and up of the frame camera that looks along -direction.
void GE_ImpostorFrameAxes(vec3 direction, out vec3 right, out vec3 up)
{
    vec3 worldUp = abs(direction.y) > 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 forward = -direction;
    right = normalize(cross(worldUp, forward));
    up = cross(forward, right);
}

#endif
