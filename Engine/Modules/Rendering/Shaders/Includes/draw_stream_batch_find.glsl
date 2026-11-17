#ifndef GE_DRAW_STREAM_BATCH_FIND_GLSL
#define GE_DRAW_STREAM_BATCH_FIND_GLSL

// Binary search of `batchTable[0, batchCount)` for the row (materialIndex, meshIndex).
// Include after draw_stream_batch_table.glsl and after the declaration of `batchTable[]`.
uint ge_FindBatch(uint materialIndex, uint meshIndex, uint batchCount) {
    uint lo = 0u;
    uint hi = batchCount;
    while (lo < hi) {
        uint mid = (lo + hi) / 2u;
        uint bMat = batchTable[mid].materialIndex;
        uint bMesh = batchTable[mid].meshIndex;
        if (bMat < materialIndex || (bMat == materialIndex && bMesh < meshIndex)) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < batchCount
        && batchTable[lo].materialIndex == materialIndex
        && batchTable[lo].meshIndex == meshIndex) {
        return lo;
    }
    return kBatchNotFound;
}

#endif
