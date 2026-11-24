// Impostor frames (Engine/Rendering/Impostors). custom0.xy of an instance is the octahedral code of
// its view direction (from the object toward the camera, object space). The frames sit on the
// grid points of that code, edge to edge (Mathematics::OctahedralFrameDirection), so the outer
// frames of the hemisphere look from the horizon. A card faces the view direction itself and
// blends the three frames around it, each reprojected onto the card. The axes follow
// MakeImpostorFrameAxes (ImpostorFrameCamera.cpp).

#ifndef GE_IMPOSTOR_FRAME_GLSL
#define GE_IMPOSTOR_FRAME_GLSL
#include "octahedral_normal.glsl"

// impostorAtlas: x = frames per side, y = layout (0 hemi-octahedral around +Y, 1 octahedral).
vec3 GE_ImpostorDecode(vec2 code, vec4 impostorAtlas)
{
    return impostorAtlas.y < 0.5 ? GE_HemiOctDecode(code) : GE_OctDecode(code);
}

// The zenith is the midpoint of the central cell's diagonal.
const vec2 kGE_ImpostorPoleFrame = vec2(3.5);

vec4 GE_ImpostorFrameRect(vec2 frame, vec4 impostorAtlas)
{
    if (impostorAtlas.y < 0.0)
    {
        if (frame.y == kGE_ImpostorPoleFrame.y)
            return vec4(4.5, 3.0, 0.75, 1.0);
        if (frame.y == 3.0 && frame.x >= 3.0 && frame.x <= 5.0)
        {
            float packedColumn = frame.x - 3.0 + (frame.x == 5.0 ? 1.0 : 0.0);
            return vec4(3.0 + packedColumn * 0.75, 3.0, 0.75, 1.0);
        }
    }
    return vec4(frame, 1.0, 1.0);
}

vec3 GE_ImpostorFrameDirection(vec2 frame, vec4 impostorAtlas)
{
    return GE_ImpostorDecode(frame / max(impostorAtlas.x - 1.0, 1.0), impostorAtlas);
}

// The card center and viewer are relative to the render origin. The viewer is a point
// (w = 1) or the direction toward the viewer (w = 0).
vec3 GE_ImpostorTowardViewer(vec4 viewer, vec3 centerRel)
{
    return viewer.w > 0.5 ? viewer.xyz - centerRel : viewer.xyz;
}

// Right and up of a camera that looks along -direction.
void GE_ImpostorFrameAxes(vec3 direction, out vec3 right, out vec3 up)
{
    vec3 worldUp = abs(direction.y) > 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 forward = -direction;
    right = normalize(cross(worldUp, forward));
    up = cross(forward, right);
}

// The three frames around a view code and their weights: the grid triangle the code falls in.
void GE_ImpostorBlendFrames(vec2 viewCode, vec4 impostorAtlas, out vec2 frames[3], out vec3 weights)
{
    float last = max(impostorAtlas.x - 1.0, 1.0);
    vec2 grid = clamp(viewCode, vec2(0.0), vec2(1.0)) * last;
    vec2 cell = min(floor(grid), vec2(last - 1.0));
    vec2 f = grid - cell;
    if (f.x + f.y < 1.0)
    {
        frames[0] = cell;
        frames[1] = cell + vec2(1.0, 0.0);
        frames[2] = cell + vec2(0.0, 1.0);
        weights = vec3(1.0 - f.x - f.y, f.x, f.y);
    }
    else
    {
        frames[0] = cell + vec2(1.0, 1.0);
        frames[1] = cell + vec2(0.0, 1.0);
        frames[2] = cell + vec2(1.0, 0.0);
        weights = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
    }
    if (impostorAtlas.y < 0.0 && all(equal(cell, vec2(3.0))))
    {
        // Add the pole on the shared diagonal. Keep the outer corner and
        // replace the smaller diagonal weight with the center contribution.
        float centerWeight = min(weights.y, weights.z);
        if (weights.y < weights.z)
        {
            frames[1] = kGE_ImpostorPoleFrame;
            weights.y = 2.0 * centerWeight;
            weights.z -= centerWeight;
        }
        else
        {
            frames[2] = kGE_ImpostorPoleFrame;
            weights.z = 2.0 * centerWeight;
            weights.y -= centerWeight;
        }
    }
}

// The atlas UV of object-space point `offset` (from the sphere center, in sphere radii) seen by
// `frame`: the frame camera's orthographic projection into its tile. UV (0,0) is the tile's
// top-left; the top of the baked image is the frame camera's up.
vec2 GE_ImpostorFrameUv(vec4 rect, vec3 frameDirection, vec3 offset, float gridSize)
{
    vec3 right;
    vec3 up;
    GE_ImpostorFrameAxes(frameDirection, right, up);
    vec2 tile = clamp(vec2(dot(offset, right) * 0.5 + 0.5, 0.5 - dot(offset, up) * 0.5), vec2(0.0), vec2(1.0));
    return (rect.xy + tile * rect.zw) / gridSize;
}

#endif
