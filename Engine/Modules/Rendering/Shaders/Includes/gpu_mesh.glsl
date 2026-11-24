#ifndef GE_GPU_MESH_GLSL
#define GE_GPU_MESH_GLSL

// The GPUScene mesh row, the GLSL mirror of GPUScene.h::GPUMesh.

const uint kMaxMeshLODs = 4u;

// 128-byte std430 layout. The field order MUST match GPUScene.h::GPUMesh.
struct GPUMesh {
    vec3  boundingCenter;
    float boundingRadius;
    float lodCoverageScale;
    // Impostor link: 0 = none, else card material index + 1 in bits 0..23 and bit 31 for the
    // primary part. MUST match GPUScene.h kGPUMeshImpostor*.
    uint  impostorCard;
    uint  vertexOffset;
    // Local radius of the card's sphere (0 = the instance sphere). MUST match GPUScene.h.
    float impostorRadius;
    uint  indexOffset;
    uint  indexCount;
    uint  lodCount;
    uint  nextImpostorPart;
    float impostorSwitchCoverage;
    uint  vertexFlags;
    uint  lodIndexOffset[kMaxMeshLODs];
    uint  lodIndexCount[kMaxMeshLODs];
    float lodThreshold[kMaxMeshLODs];
    // Per-LOD vertex offset RELATIVE to vertexOffset (authored LODs; 0 for
    // generated levels). Draw emits vertexOffset + lodVertexOffset[lod].
    uint  lodVertexOffset[kMaxMeshLODs];
    // LOD selection flags: bits 0..3 = lodThreshold[k] is SSE-normalized
    // (scale it by sseThresholdToCoverage to get its coverage-space switch
    // point); bit 4 = tight class (skinned chain) takes the tighter per-view
    // factor. MUST match GPUScene.h kGPUMeshLodSseSlotMask /
    // kGPUMeshLodTightClassBit.
    uint  lodFlags;
    // Per-mesh ceiling on sseThresholdToCoverage (MeshLODThresholds.h::
    // LodSseScaleCeil). 0 = no ceiling.
    float lodSseScaleCeil;
};

#endif
