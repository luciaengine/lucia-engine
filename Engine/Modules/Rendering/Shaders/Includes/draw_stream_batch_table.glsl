#ifndef GE_DRAW_STREAM_BATCH_TABLE_GLSL
#define GE_DRAW_STREAM_BATCH_TABLE_GLSL

// The batch table that the draw stream kernels search, and the keys an instance searches it with.
// Include draw_stream_batch_find.glsl after the declaration of `batchTable[]`.

// Mirrors GPUDrawStreamBuilder::BatchTableEntry (16 B std430). Rows sort by (materialIndex, meshIndex).
struct BatchTableEntry {
    uint materialIndex;
    uint meshIndex;
    uint recordOffset;   // slice-relative exclusive prefix of capacities
    uint capacity;       // snapshot live-instance count (the per-batch draw bound)
};

const uint kBatchNotFound = 0xFFFFFFFFu;

// GPUInstance.flags depth-class bits and the two shadow-table class sentinels.
// MUST match Rendering::GPUInstanceDepthClass.h and GPUDrawStreamBuilder.
const uint kDepthMaterialDependentBit = 4u;        // flags bit 2
const uint kDepthDoubleSidedBit       = 8u;        // flags bit 3
const uint kMirroredBit               = 16u;       // flags bit 4 (negative-det world transform)
const uint kSharedDepthSSSentinel     = 0xFFFFFFu; // single-sided class group
const uint kSharedDepthDSSentinel     = 0xFFFFFEu; // double-sided class group

// Bit 24 of the batch-table meshIndex field marks the mirrored sibling row. Real mesh indices are
// 24-bit. MUST match GPUDrawStreamBuilder::kMeshParityBit.
const uint kMeshParityBit             = 0x1000000u;

// The mesh-axis value of a mesh with no draw-consolidation group. MUST match
// MeshPoolGroupPlan::kAbsentGroup. Its rows are never drawn.
const uint kAbsentPoolGroup           = 0xFFFFFFu;

// The shadow-table material key of an instance. A material-dependent caster keeps its material.
// Any other caster searches the shared single-sided or double-sided class row of its mesh.
uint ge_ShadowSearchMaterial(uint flags, uint materialIndex) {
    if ((flags & kDepthMaterialDependentBit) != 0u)
        return materialIndex;
    return ((flags & kDepthDoubleSidedBit) != 0u) ? kSharedDepthDSSentinel : kSharedDepthSSSentinel;
}

// The shadow-table parity bit of an instance. A double-sided caster draws both faces, and the CPU
// table never splits its row by parity, so it always searches parity 0.
uint ge_ShadowParityBit(uint flags) {
    if ((flags & kMirroredBit) == 0u || (flags & kDepthDoubleSidedBit) != 0u)
        return 0u;
    return kMeshParityBit;
}

#endif
