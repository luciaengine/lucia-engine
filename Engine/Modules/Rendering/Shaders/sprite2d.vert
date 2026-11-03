#version 450

layout(set = 0, binding = 5) uniform CameraUBO {
#include "Includes/camera_ubo_fields.glsl"
} Cam;

// One instanced quad per visible Sprite2D / AnimatedSprite2D. The instance
// carries the quad's world-space frame (centre plus half-extent axes taken from
// the entity's local X/Y columns), so the transform's rotation and scale apply
// without a per-sprite matrix multiply here.
//
// Sprites lie in the entity's local XY plane with +Y up (the tilemap
// convention); texture V grows downward, so the V lerp is inverted below.

struct SpriteInstance
{
    vec4 Center; // xyz = quad centre in world space, w unused
    vec4 AxisX;  // xyz = world half-extent along the sprite's local +X
    vec4 AxisY;  // xyz = world half-extent along the sprite's local +Y
    vec4 UVRect; // (u0, v0, u1, v1), flips already applied by the extractor
    vec4 Tint;   // linear rgba
    vec4 Surface; // x = lit, y = normal strength, z = has a normal map, w = light count
};

layout(std430, set = 0, binding = 0) readonly buffer Instances
{
    SpriteInstance uInstances[];
};

layout(push_constant) uniform SpritePush
{
    // x = 1 / exposure the tonemap will apply to this view. Sprites are
    // display-referred art drawn into the scene-linear HDR buffer, so
    // pre-dividing keeps a white texel white whatever the scene luminance is.
    vec4 InvExposure;
    // x = index of this batch's first instance. Batches are runs within one
    // shared buffer bound whole: a per-batch descriptor offset would have to
    // meet minStorageBufferOffsetAlignment, which a 96-byte stride does not.
    uvec4 Batch;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vTint;
layout(location = 2) out vec3 vPosition;
layout(location = 3) out vec3 vTangent;
layout(location = 4) out vec3 vBitangent;
layout(location = 5) out vec3 vNormal;
layout(location = 6) flat out vec4 vSurface;

// Two triangles over the [-1,1] quad, counter-clockwise in the sprite's
// +X right / +Y up frame.
const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main()
{
    SpriteInstance inst = uInstances[pc.Batch.x + uint(gl_InstanceIndex)];
    const vec2 corner = kCorners[gl_VertexIndex];

    const vec3 worldPos = inst.Center.xyz + corner.x * inst.AxisX.xyz + corner.y * inst.AxisY.xyz;
    gl_Position = Cam.uVP * vec4(worldPos, 1.0);

    // The tangent frame follows the texture: a flipped rect mirrors its axis,
    // so the normal map's +X and +Y still point where the art says. The face
    // looks along -(X cross Y), towards a camera in front of the sprite.
    vPosition = worldPos;
    vTangent = normalize(inst.AxisX.xyz) * (inst.UVRect.z >= inst.UVRect.x ? 1.0 : -1.0);
    vBitangent = normalize(inst.AxisY.xyz) * (inst.UVRect.w >= inst.UVRect.y ? 1.0 : -1.0);
    vNormal = -normalize(cross(inst.AxisX.xyz, inst.AxisY.xyz));
    vSurface = inst.Surface;

    // corner maps [-1,1] -> [0,1]; t.y runs bottom-to-top over the quad while
    // the rect's w is the frame's bottom edge in texture space.
    const vec2 t = corner * 0.5 + 0.5;
    vUV = vec2(mix(inst.UVRect.x, inst.UVRect.z, t.x), mix(inst.UVRect.w, inst.UVRect.y, t.y));
    // Alpha is coverage, so exposure compensation touches rgb only. A lit
    // sprite's colour is an albedo lit in scene-linear units, so the tonemap's
    // exposure applies to it unchanged, as it does to a mesh.
    const float exposure = inst.Surface.x > 0.5 ? 1.0 : pc.InvExposure.x;
    vTint = vec4(inst.Tint.rgb * exposure, inst.Tint.a);
}
