#pragma once

#include "Assets/ModelAsset.h"

namespace GameEngine
{

struct MeshClusterLODConfig
{
    uint32 MeshletVertices = 64;
    uint32 MeshletTriangles = 124;
    uint32 MeshletsPerGroup = 32;
    uint32 MaximumGroups = 4096;
    uint64 MaximumGeometryBytes = 256ull * 1024ull * 1024ull;
    uint64 MaximumMetadataBytes = 16ull * 1024ull * 1024ull;
    // Bounds requested element bytes of explicitly sized cooker arrays, not
    // allocator capacity/RSS, source/prior output, strings or meshoptimizer's allocator.
    uint64 MaximumScratchBytes = 256ull * 1024ull * 1024ull;
    float CoarseRatio = 0.25f;
    float RelativeError = 0.02f;
};

struct MeshClusterLODGroup
{
    Mesh Coarse;
    Mesh Fine;
    // Meshoptimizer's positional/attribute error estimate in mesh-local units.
    float Error = 0.0f;
};

/// Partitions one static triangle submesh and locks every shared group boundary.
/// On failure, leaves groups unchanged and reports the refusal or cancellation.
bool GenerateMeshClusterLODs(const Mesh& mesh, const MeshClusterLODConfig& config,
                            Vector<MeshClusterLODGroup>& groups, String& error);

} // namespace GameEngine
