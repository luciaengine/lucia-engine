#ifndef GE_GPU_MESH_GLSL
#define GE_GPU_MESH_GLSL

// The GPUScene mesh row, the GLSL mirror of GPUScene.h::GPUMesh.

const uint kMaxMeshLODs = 4u;

// 128-byte std430 layout. The field order MUST match GPUScene.h::GPUMesh.
struct GPUMesh {
    vec3  boundingCenter;
    float boundingRadius;
    float lodCoverageScale;
    uint  clusterCount;
    uint  vertexOffset;
    uint  vertexCount;
    uint  indexOffset;
    uint  indexCount;
    uint  lodCount;
    uint  bucketKey;
    uint  indexType;
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
