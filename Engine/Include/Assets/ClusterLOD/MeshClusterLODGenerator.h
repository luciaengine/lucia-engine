#pragma once

#include "Assets/ModelAsset.h"
#include "Mathematics/Vector3.h"

#include <span>

namespace GameEngine
{

struct MeshClusterLODConfig
{
    uint32 MeshletVertices = 64;
    uint32 MeshletTriangles = 124;
    // The partition target; meshoptimizer makes groups of up to a third more clusters.
    uint32 MeshletsPerGroup = 32;
    // The DAG groups of one source mesh.
    uint32 MaximumGroups = 8192;
    uint64 MaximumGeometryBytes = 256ull * 1024ull * 1024ull;
    uint64 MaximumMetadataBytes = 16ull * 1024ull * 1024ull;
    // Bounds requested element bytes of explicitly sized cooker arrays, not
    // allocator capacity/RSS, source/prior output, strings or meshoptimizer's allocator.
    uint64 MaximumScratchBytes = 512ull * 1024ull * 1024ull;
    // The resident cut (LOD0) is the coarsest DAG level that has at most ResidentRatio of the
    // source triangles and an error of at most ResidentRelativeError of the source mesh extent.
    // A mesh whose levels simplify badly (sharp, boxy shapes) keeps a finer LOD0, or its source.
    float ResidentRatio = 0.25f;
    float ResidentRelativeError = 0.02f;
    // Cutout coverage depends on the UVs. Use only attribute-aware simplification for those meshes.
    bool AllowPositionOnlyReduction = true;
};

/// The group value of a unit made from source triangles.
inline constexpr uint32 kMeshClusterLODSourceGroup = ~0u;

/// One group of the cluster DAG: clusters merged and simplified together.
struct MeshClusterLODDagGroup
{
    // The sphere encloses the spheres of every cluster of the group, so it encloses the spheres
    // of every finer group under it. The projected error is then monotone up the DAG.
    Mathematics::Vector3 Center;
    float Radius = 0.0f;
    // The error of the simplified replacement in mesh units: at least the error of every cluster
    // in the group. Not used for a terminal group.
    float Error = 0.0f;
    // The DAG level: 0 for groups of source clusters. A unit's group is one level above its producer.
    uint32 Depth = 0;
    // The group did not simplify. Its clusters draw in every cut coarser than their producer.
    bool Terminal = false;
};

/// The clusters of one group that one producer group made. They draw and hide together: a unit
/// draws when its group is refined and its producer is not. A terminal group is always refined.
struct MeshClusterLODDagUnit
{
    uint32 Group = 0;
    uint32 Producer = kMeshClusterLODSourceGroup;
    // Triangles over the source mesh vertices.
    Vector<uint32> Indices;
};

struct MeshClusterLODDag
{
    Vector<MeshClusterLODDagGroup> Groups;
    // Sorted by group, then producer.
    Vector<MeshClusterLODDagUnit> Units;
};

/// Builds the cluster DAG of one static triangle submesh: partition the clusters into groups,
/// lock shared group borders, simplify each group to about half, re-cluster, until one cluster
/// is left or every group stops reducing. Non-manifold edges and vertices are locked.
/// On failure, leaves `dag` unchanged and reports the refusal or cancellation.
bool GenerateMeshClusterLODDag(const Mesh& mesh, const MeshClusterLODConfig& config, MeshClusterLODDag& dag,
                               String& error);

/// The units of the uniform cut at DAG level `depth`: every group under that level is not
/// refined and every group at or over it is refined. Level 0 is the source.
bool IsMeshClusterLODUnitInLevel(uint32 unitGroupDepth, bool unitGroupTerminal, uint32 depth);

/// The `remap` entry of a source vertex that the compacted mesh does not use.
inline constexpr uint32 kMeshClusterLODUnusedVertex = ~0u;

/// Copies name, material and node placement from `source` to `destination`.
void CopyMeshClusterLODPlacement(const Mesh& source, Mesh& destination);

/// The triangles `indices` of `source` as a mesh with only the vertices they use; bounds cover
/// those vertices. `remap` has one kMeshClusterLODUnusedVertex entry per source vertex; the call
/// restores it. `sourceVertices` receives the source index of each output vertex.
Mesh CompactMeshClusterLODTriangles(const Mesh& source, std::span<const uint32> indices,
                                   std::span<uint32> remap, Vector<uint32>& sourceVertices);

} // namespace GameEngine
