#version 450

layout(set = 0, binding = 5) uniform CameraUBO {
#include "Includes/camera_ubo_fields.glsl"
} Cam;

// One instanced quad per non-empty cell of one chunk. The instance stream is
// the CPU-compacted list (empty cells are never instanced), so gl_InstanceIndex
// addresses occupied cells directly.
//
// Tiles lie in the tilemap's local XY plane with +Y up (engine convention);
// the atlas V axis grows downward, so the V lerp is inverted below.

struct TileInstance
{
    uint LocalIndex; // cell index within the 32x32 chunk (y * 32 + x)
    uint Cell;       // packed TileCell: bits 0..28 tile id, 29 flipH, 30 flipV, 31 transpose
};

layout(std430, set = 0, binding = 0) readonly buffer Instances
{
    TileInstance uInstances[];
};

struct TileDesc
{
    vec4 UV;   // (u0, v0, u1, v1)
    vec4 Anim; // x = frame count (0 = static), y = first frame index, z = loop duration
};

layout(std430, set = 0, binding = 1) readonly buffer TileDescs
{
    TileDesc uTiles[]; // per tile id - 1
};

layout(std430, set = 0, binding = 3) readonly buffer AnimFrames
{
    vec2 uFrames[]; // x = uv-source tile index (0-based), y = loop time the frame ends at
};

layout(push_constant) uniform TileMapPush
{
    mat4 Model;
    vec4 ChunkOriginCellSize; // xy = chunk origin in tilemap space, zw = cell size
    vec4 LayerZ;              // x = layer Z in tilemap space, y = animation clock (seconds),
                              // z = tile count as a bit pattern (floatBitsToUint)
    vec4 Tint;                // rgba, opacity already folded into a
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vTint;

const uint kCellTileIdMask = 0x1FFFFFFFu;
const uint kCellFlipHBit = 0x20000000u;
const uint kCellFlipVBit = 0x40000000u;
const uint kCellTransposeBit = 0x80000000u;

const uint kChunkSize = 32u;

// Two triangles, counter-clockwise in the tilemap's +X right / +Y up frame.
const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                 vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main()
{
    TileInstance inst = uInstances[gl_InstanceIndex];

    const uint tileId = inst.Cell & kCellTileIdMask;
    const vec2 corner = kCorners[gl_VertexIndex];

    // A cell's 29-bit id is data, not a promise: a re-sliced tileset, an
    // importer's high gid or a hand-edited .tilemap can all name a tile the
    // descriptor table does not have. Collapse the quad to a point instead of
    // reading past the buffer.
    const uint tileCount = floatBitsToUint(pc.LayerZ.z);
    if (tileId == 0u || tileId > tileCount)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        vUV = vec2(0.0);
        vTint = vec4(0.0);
        return;
    }

    const vec2 localCell = vec2(float(inst.LocalIndex % kChunkSize),
                                float(inst.LocalIndex / kChunkSize));
    const vec2 localXY =
        pc.ChunkOriginCellSize.xy + (localCell + corner) * pc.ChunkOriginCellSize.zw;

    gl_Position = Cam.uVP * pc.Model * vec4(localXY, pc.LayerZ.x, 1.0);

    // tileId is 1-based and bounds-checked above.
    TileDesc desc = uTiles[tileId - 1u];
    vec4 rect = desc.UV;

    // Animated tiles pick their frame from the shared clock, so every instance
    // of a tile is in phase (LDtk/Tiled behavior). CPU mirror: TileAnimation.h.
    const uint frameCount = uint(desc.Anim.x);
    if (frameCount > 0u)
    {
        const float t = mod(pc.LayerZ.y, desc.Anim.z);
        const uint firstFrame = uint(desc.Anim.y);
        for (uint i = 0u; i < frameCount; ++i)
        {
            const vec2 frame = uFrames[firstFrame + i];
            if (t < frame.y || i == frameCount - 1u)
            {
                rect = uTiles[uint(frame.x)].UV;
                break;
            }
        }
    }

    vec2 t = corner;
    if ((inst.Cell & kCellTransposeBit) != 0u)
        t = t.yx;
    if ((inst.Cell & kCellFlipHBit) != 0u)
        t.x = 1.0 - t.x;
    if ((inst.Cell & kCellFlipVBit) != 0u)
        t.y = 1.0 - t.y;

    // t.y runs bottom-to-top over the quad; V1 is the tile's bottom edge.
    vUV = vec2(mix(rect.x, rect.z, t.x), mix(rect.w, rect.y, t.y));
    vTint = pc.Tint;
}
