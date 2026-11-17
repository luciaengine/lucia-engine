#include "Assets/ClusterLOD/MeshClusterLODGenerator.h"

#include "Assets/AssetDecodeCancellation.h"
#include "Mathematics/Geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#if defined(GE_HAVE_MESHOPTIMIZER)
#include <meshoptimizer.h>
#endif

namespace GameEngine
{
namespace
{

constexpr uint64 kMaximumGeometryBytes = 256ull * 1024ull * 1024ull;
constexpr uint64 kMaximumMetadataBytes = 16ull * 1024ull * 1024ull;
constexpr uint64 kMaximumScratchBytes = 1024ull * 1024ull * 1024ull;
constexpr uint32 kMaximumGroups = 65536;
// Leave headroom for meshoptimizer's products of float coordinates and attributes.
constexpr float kMaximumComponent = 100000000.0f;
// A meshlet with large bounds splits when it grows past this factor of the expected size.
constexpr float kMeshletSplitFactor = 2.0f;
// A meshlet may stop at this fraction of MeshletTriangles to stay spatially compact.
constexpr uint32 kMeshletMinimumTriangleDivisor = 3;
// Each level aims at this fraction of the triangles of the group.
constexpr float kSimplifyRatio = 0.5f;
// A group whose simplification keeps more than this fraction of its triangles is terminal.
constexpr float kStuckRatio = 0.85f;
// The sloppy fallback ignores topology, so its error counts double.
constexpr float kSloppyErrorFactor = 2.0f;
// A DAG of a valid mesh halves its triangles per level; this many levels covers any source.
constexpr uint32 kMaximumDepth = 64;
// Normal, then texture coordinates.
constexpr uint32 kAttributeCount = 5;
constexpr float kAttributeWeights[kAttributeCount] = {2.0f, 2.0f, 2.0f, 0.5f, 0.5f};

bool ValidComponents(std::span<const float> values)
{
    return std::all_of(values.begin(), values.end(), [](float value)
    { return std::isfinite(value) && std::abs(value) <= kMaximumComponent; });
}

bool ValidateInput(const Mesh& mesh, const MeshClusterLODConfig& config, String& error)
{
    if (config.MeshletVertices < 3 || config.MeshletVertices > 256 ||
        config.MeshletTriangles < 4 || config.MeshletTriangles > 512 ||
        config.MeshletTriangles % 4 != 0 || config.MeshletsPerGroup < 2 ||
        config.MeshletsPerGroup > 128 || config.MaximumGroups == 0 ||
        config.MaximumGroups > kMaximumGroups || config.MaximumGeometryBytes == 0 ||
        config.MaximumGeometryBytes > kMaximumGeometryBytes ||
        config.MaximumMetadataBytes == 0 || config.MaximumMetadataBytes > kMaximumMetadataBytes ||
        config.MaximumScratchBytes == 0 || config.MaximumScratchBytes > kMaximumScratchBytes ||
        !std::isfinite(config.ResidentRatio) || config.ResidentRatio <= 0.0f || config.ResidentRatio >= 1.0f)
    {
        error = "Invalid cluster LOD limits";
        return false;
    }
    if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles || mesh.Vertices.empty() ||
        mesh.Indices.empty() || mesh.Indices.size() % 3 != 0 || mesh.HasAuthoredLODs() ||
        mesh.Skinned || !mesh.Joints0.empty() || !mesh.Weights0.empty() ||
        !mesh.Joints1.empty() || !mesh.Weights1.empty() || mesh.HasMorphTargets() ||
        !mesh.Color0.empty() || !mesh.TexCoords1.empty() || !mesh.ExtraTexCoords.empty())
    {
        error = "Cluster LOD requires static triangles without authored LODs or optional vertex streams";
        return false;
    }
    if (mesh.Vertices.size() > config.MaximumGeometryBytes / sizeof(Vertex) ||
        mesh.Indices.size() > (config.MaximumGeometryBytes - mesh.Vertices.size() * sizeof(Vertex)) /
                                  sizeof(uint32))
    {
        error = "Cluster source exceeds the geometry limit";
        return false;
    }
    if (mesh.Name.size() > 1024 || mesh.ExtraPlacements.size() > 1024 ||
        !ValidComponents(mesh.SourceNodeTransform) || !ValidComponents(mesh.SourceNodeLocalTransform))
    {
        error = "Cluster source has invalid placement metadata";
        return false;
    }
    for (const MeshPlacement& placement : mesh.ExtraPlacements)
    {
        if (!ValidComponents(placement.SourceNodeTransform) || !ValidComponents(placement.SourceNodeLocalTransform))
        {
            error = "Cluster source has invalid placement metadata";
            return false;
        }
    }
    for (const Vertex& vertex : mesh.Vertices)
    {
        if (!ValidComponents(vertex.Position) || !ValidComponents(vertex.Normal) ||
            !ValidComponents(vertex.TexCoords) || !ValidComponents(vertex.Tangent))
        {
            error = "Cluster source has invalid vertex components";
            return false;
        }
    }
    for (size_t offset = 0; offset < mesh.Indices.size(); offset += 3)
    {
        const uint32 a = mesh.Indices[offset];
        const uint32 b = mesh.Indices[offset + 1];
        const uint32 c = mesh.Indices[offset + 2];
        if (a >= mesh.Vertices.size() || b >= mesh.Vertices.size() || c >= mesh.Vertices.size())
        {
            error = "Cluster source has an out-of-range index";
            return false;
        }
        const auto& first = mesh.Vertices[a].Position;
        const auto& second = mesh.Vertices[b].Position;
        const auto& third = mesh.Vertices[c].Position;
        const Mathematics::Vector3 edgeA{second[0] - first[0], second[1] - first[1], second[2] - first[2]};
        const Mathematics::Vector3 edgeB{third[0] - first[0], third[1] - first[1], third[2] - first[2]};
        if (Mathematics::Vector3::Cross(edgeA, edgeB).LengthSquared() <= 0.0f)
        {
            error = "Cluster source has a degenerate triangle";
            return false;
        }
    }
    return true;
}

#if defined(GE_HAVE_MESHOPTIMIZER)

uint64 EdgeKey(uint32 a, uint32 b)
{
    return (static_cast<uint64>(std::min(a, b)) << 32) | std::max(a, b);
}

struct SourceEdge
{
    uint64 Key;
    uint32 LowCorner;
    uint32 HighCorner;
};

uint32 FindCornerRoot(Vector<uint32>& parents, uint32 corner)
{
    while (parents[corner] != corner)
    {
        parents[corner] = parents[parents[corner]];
        corner = parents[corner];
    }
    return corner;
}

void JoinCorners(Vector<uint32>& parents, uint32 a, uint32 b)
{
    const uint32 rootA = FindCornerRoot(parents, a);
    const uint32 rootB = FindCornerRoot(parents, b);
    parents[std::max(rootA, rootB)] = std::min(rootA, rootB);
}

// The corner of the triangle of `low` and `high` that is neither of them.
uint32 ThirdCorner(uint32 low, uint32 high)
{
    return low - low % 3 + 3 - low % 3 - high % 3;
}

void LockEdge(Vector<uint8>& locks, uint64 key)
{
    locks[static_cast<uint32>(key >> 32)] |= meshopt_SimplifyVertex_Lock;
    locks[static_cast<uint32>(key)] |= meshopt_SimplifyVertex_Lock;
}

// Maps each vertex to the first vertex at its position and marks the positions the simplifier
// must keep: open source edges, UV seams (protected), and every non-manifold edge or vertex.
void LockSourcePositions(const Mesh& mesh, Vector<uint32>& positions, Vector<uint8>& locks)
{
    positions.resize(mesh.Vertices.size());
    {
        // -0 and +0 are one position.
        Vector<Mathematics::Vector3> normalized;
        normalized.reserve(mesh.Vertices.size());
        for (const Vertex& vertex : mesh.Vertices)
            normalized.push_back({vertex.Position[0] == 0.0f ? 0.0f : vertex.Position[0],
                                  vertex.Position[1] == 0.0f ? 0.0f : vertex.Position[1],
                                  vertex.Position[2] == 0.0f ? 0.0f : vertex.Position[2]});
        meshopt_generatePositionRemap(positions.data(), &normalized.front().x, normalized.size(),
                                     sizeof(Mathematics::Vector3));
    }
    locks.assign(mesh.Vertices.size(), 0);
    Vector<uint32> firstVertex(mesh.Vertices.size(), kMeshClusterLODUnusedVertex);
    // A UV seam and a tangent handedness seam keep their discontinuity. Other wedges (hard
    // normals) may collapse across.
    for (uint32 index = 0; index < mesh.Vertices.size(); ++index)
    {
        const uint32 position = positions[index];
        const uint32 previous = firstVertex[position];
        const Vertex& vertex = mesh.Vertices[index];
        if (previous != kMeshClusterLODUnusedVertex &&
            (!std::equal(std::begin(vertex.TexCoords), std::end(vertex.TexCoords),
                         std::begin(mesh.Vertices[previous].TexCoords)) ||
             vertex.Tangent[3] != mesh.Vertices[previous].Tangent[3]))
            locks[position] |= meshopt_SimplifyVertex_Protect;
        firstVertex[position] = index;
    }
    Vector<SourceEdge> edges;
    edges.reserve(mesh.Indices.size());
    Vector<uint32> parents(mesh.Indices.size());
    for (uint32 corner = 0; corner < parents.size(); ++corner)
        parents[corner] = corner;
    for (size_t offset = 0; offset < mesh.Indices.size(); offset += 3)
    {
        for (uint32 edge = 0; edge < 3; ++edge)
        {
            const uint32 a = static_cast<uint32>(offset) + edge;
            const uint32 b = static_cast<uint32>(offset) + (edge + 1) % 3;
            const uint32 positionA = positions[mesh.Indices[a]];
            const uint32 positionB = positions[mesh.Indices[b]];
            edges.push_back({EdgeKey(positionA, positionB),
                             positionA < positionB ? a : b, positionA < positionB ? b : a});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const SourceEdge& a, const SourceEdge& b) { return a.Key < b.Key; });
    for (size_t begin = 0; begin < edges.size();)
    {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end].Key == edges[begin].Key)
            ++end;
        if (end - begin != 2)
        {
            // An open edge, or an edge of more than two triangles.
            LockEdge(locks, edges[begin].Key);
            begin = end;
            continue;
        }
        const SourceEdge& first = edges[begin];
        const SourceEdge& second = edges[begin + 1];
        const uint32 firstThird = positions[mesh.Indices[ThirdCorner(first.LowCorner, first.HighCorner)]];
        const uint32 secondThird = positions[mesh.Indices[ThirdCorner(second.LowCorner, second.HighCorner)]];
        const bool firstForward = first.HighCorner % 3 == (first.LowCorner % 3 + 1) % 3;
        const bool secondForward = second.HighCorner % 3 == (second.LowCorner % 3 + 1) % 3;
        if (firstThird == secondThird || firstForward == secondForward)
        {
            // A duplicate face or inconsistent winding.
            LockEdge(locks, first.Key);
            locks[firstThird] |= meshopt_SimplifyVertex_Lock;
            locks[secondThird] |= meshopt_SimplifyVertex_Lock;
        }
        else
        {
            JoinCorners(parents, first.LowCorner, second.LowCorner);
            JoinCorners(parents, first.HighCorner, second.HighCorner);
        }
        begin = end;
    }
    // A position whose corners form more than one fan is a non-manifold vertex.
    std::fill(firstVertex.begin(), firstVertex.end(), kMeshClusterLODUnusedVertex);
    for (uint32 corner = 0; corner < mesh.Indices.size(); ++corner)
    {
        const uint32 position = positions[mesh.Indices[corner]];
        const uint32 root = FindCornerRoot(parents, corner);
        if (firstVertex[position] != kMeshClusterLODUnusedVertex && firstVertex[position] != root)
            locks[position] |= meshopt_SimplifyVertex_Lock;
        firstVertex[position] = root;
    }
}

// True when every triangle has a positive area.
bool AllTrianglesHaveArea(const Mesh& mesh, std::span<const uint32> indices)
{
    for (size_t offset = 0; offset < indices.size(); offset += 3)
    {
        const auto& first = mesh.Vertices[indices[offset]].Position;
        const auto& second = mesh.Vertices[indices[offset + 1]].Position;
        const auto& third = mesh.Vertices[indices[offset + 2]].Position;
        const Mathematics::Vector3 edgeA{second[0] - first[0], second[1] - first[1], second[2] - first[2]};
        const Mathematics::Vector3 edgeB{third[0] - first[0], third[1] - first[1], third[2] - first[2]};
        if (Mathematics::Vector3::Cross(edgeA, edgeB).LengthSquared() <= 0.0f)
            return false;
    }
    return true;
}

// Removes triangles with two corners at one position: wedges that a collapse joined. Such a
// triangle has no area and no open edge, so the surface stays closed.
void RemovePositionDegenerates(Vector<uint32>& indices, std::span<const uint32> positions)
{
    size_t kept = 0;
    for (size_t offset = 0; offset < indices.size(); offset += 3)
    {
        const uint32 first = positions[indices[offset]];
        const uint32 second = positions[indices[offset + 1]];
        const uint32 third = positions[indices[offset + 2]];
        if (first == second || second == third || third == first)
            continue;
        std::copy_n(indices.begin() + offset, 3, indices.begin() + kept);
        kept += 3;
    }
    indices.resize(kept);
}

// The edges of the triangles that are not shared by exactly two triangles, as position-pair keys
// with their use count. A simplified group with the same border edges joins its neighbors
// without a crack, and keeps every non-manifold edge.
Vector<std::pair<uint64, uint32>> BorderEdges(std::span<const uint32> indices, std::span<const uint32> positions)
{
    Vector<uint64> edges;
    edges.reserve(indices.size());
    for (size_t offset = 0; offset < indices.size(); offset += 3)
        for (size_t corner = 0; corner < 3; ++corner)
            edges.push_back(EdgeKey(positions[indices[offset + corner]], positions[indices[offset + (corner + 1) % 3]]));
    std::sort(edges.begin(), edges.end());
    Vector<std::pair<uint64, uint32>> border;
    for (size_t begin = 0; begin < edges.size();)
    {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end] == edges[begin])
            ++end;
        if (end - begin != 2)
            border.push_back({edges[begin], static_cast<uint32>(end - begin)});
        begin = end;
    }
    return border;
}

struct ClusterBounds
{
    Mathematics::Vector3 Center;
    float Radius = 0.0f;
    float Error = 0.0f;
};

struct Cluster
{
    size_t IndexOffset = 0;
    size_t IndexCount = 0;
    uint32 Producer = kMeshClusterLODSourceGroup;
    ClusterBounds Bounds;
};

// One run of the DAG build over one source mesh. Every level indexes the source vertices.
class DagBuilder
{
  public:
    DagBuilder(const Mesh& mesh, const MeshClusterLODConfig& config) : m_Mesh(mesh), m_Config(config) {}

    bool Build(MeshClusterLODDag& dag, String& error);

  private:
    const float* Positions() const { return m_Mesh.Vertices.front().Position; }

    bool AdmitScratch(String& error) const;
    void Clusterize(std::span<const uint32> indices, uint32 producer, const ClusterBounds* bounds);
    Vector<uint32> Partition();
    void LockGroupBorders(std::span<const uint32> offsets);
    bool Simplify(std::span<const uint32> merged, Vector<uint32>& output, float& error) const;
    bool SimplifySloppy(std::span<const uint32> merged, size_t target, Vector<uint32>& output, float& error) const;
    bool IsValidReduction(std::span<const uint32> merged, std::span<const uint32> simplified) const;
    bool EmitGroup(std::span<const Cluster> clusters, std::span<const uint32> clusterIndices,
                   const ClusterBounds& bounds, uint32 depth, bool terminal, String& error);

    const Mesh& m_Mesh;
    const MeshClusterLODConfig& m_Config;
    Vector<uint32> m_Positions;
    Vector<uint8> m_SourceLocks;
    // Per position for the current level, then per vertex for meshoptimizer.
    Vector<uint8> m_PositionLocks;
    Vector<uint8> m_VertexLocks;
    Vector<float> m_Attributes;
    Vector<Cluster> m_Clusters;
    Vector<uint32> m_ClusterIndices;
    MeshClusterLODDag m_Dag;
    uint64 m_UnitIndices = 0;
};

bool DagBuilder::AdmitScratch(String& error) const
{
    const uint64 vertices = m_Mesh.Vertices.size();
    const uint64 indices = m_Mesh.Indices.size();
    const uint64 meshlets = meshopt_buildMeshletsBound(indices, m_Config.MeshletVertices,
        std::max<size_t>(1, m_Config.MeshletTriangles / kMeshletMinimumTriangleDivisor));
    // Per vertex: position id, three lock arrays, attributes and the normalized positions. Per
    // index: two levels of cluster indices, merged, simplified, edges and corner parents, then the
    // meshlet arrays of the first clusterization and the units, which hold every level.
    const uint64 bytes = vertices * (2 * sizeof(uint32) + 3 * sizeof(uint8) + kAttributeCount * sizeof(float) +
                                     sizeof(Mathematics::Vector3)) +
        indices * (6 * sizeof(uint32) + sizeof(SourceEdge) + sizeof(uint8)) +
        meshlets * (sizeof(meshopt_Meshlet) + sizeof(Cluster)) + 2 * indices * sizeof(uint32);
    if (bytes > m_Config.MaximumScratchBytes)
    {
        error = "Cluster DAG scratch exceeds the limit";
        return false;
    }
    return true;
}

// Splits `indices` into meshlets and appends one cluster per meshlet. Source clusters get their
// own bounds; produced clusters take the bounds of the producer's simplified group.
void DagBuilder::Clusterize(std::span<const uint32> indices, uint32 producer, const ClusterBounds* bounds)
{
    const size_t minimumTriangles = std::max<size_t>(1, m_Config.MeshletTriangles / kMeshletMinimumTriangleDivisor);
    Vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(indices.size(), m_Config.MeshletVertices, minimumTriangles));
    Vector<uint32> vertices(indices.size());
    Vector<uint8> triangles(indices.size());
    const size_t count = meshopt_buildMeshletsFlex(meshlets.data(), vertices.data(), triangles.data(), indices.data(),
                                                  indices.size(), Positions(), m_Mesh.Vertices.size(), sizeof(Vertex),
                                                  m_Config.MeshletVertices, minimumTriangles, m_Config.MeshletTriangles,
                                                  0.0f, kMeshletSplitFactor);
    for (size_t index = 0; index < count; ++index)
    {
        const meshopt_Meshlet& meshlet = meshlets[index];
        Cluster cluster;
        cluster.IndexOffset = m_ClusterIndices.size();
        cluster.IndexCount = meshlet.triangle_count * 3;
        cluster.Producer = producer;
        for (size_t corner = 0; corner < cluster.IndexCount; ++corner)
            m_ClusterIndices.push_back(vertices[meshlet.vertex_offset + triangles[meshlet.triangle_offset + corner]]);
        if (bounds)
            cluster.Bounds = *bounds;
        else
        {
            const meshopt_Bounds sphere = meshopt_computeClusterBounds(
                m_ClusterIndices.data() + cluster.IndexOffset, cluster.IndexCount, Positions(), m_Mesh.Vertices.size(),
                sizeof(Vertex));
            cluster.Bounds = {{sphere.center[0], sphere.center[1], sphere.center[2]}, sphere.radius, 0.0f};
        }
        m_Clusters.push_back(cluster);
    }
}

// Sorts m_Clusters into spatial groups, each one contiguous, and returns the group offsets with
// the end last.
Vector<uint32> DagBuilder::Partition()
{
    const size_t count = m_Clusters.size();
    if (count <= m_Config.MeshletsPerGroup)
        return {0, static_cast<uint32>(count)};
    // The partition joins clusters by shared positions, so a UV seam does not become a group border.
    Vector<uint32> referenced;
    Vector<uint32> counts;
    referenced.reserve(m_ClusterIndices.size());
    counts.reserve(count);
    for (const Cluster& cluster : m_Clusters)
    {
        for (size_t corner = 0; corner < cluster.IndexCount; ++corner)
            referenced.push_back(m_Positions[m_ClusterIndices[cluster.IndexOffset + corner]]);
        counts.push_back(static_cast<uint32>(cluster.IndexCount));
    }
    Vector<uint32> partition(count);
    const size_t groups = meshopt_partitionClusters(partition.data(), referenced.data(), referenced.size(), counts.data(),
                                                    count, Positions(), m_Mesh.Vertices.size(), sizeof(Vertex),
                                                    m_Config.MeshletsPerGroup);
    Vector<uint32> offsets(groups + 1, 0);
    for (const uint32 group : partition)
        ++offsets[group + 1];
    for (size_t group = 1; group <= groups; ++group)
        offsets[group] += offsets[group - 1];
    Vector<uint32> write(offsets.begin(), offsets.end() - 1);
    Vector<Cluster> sorted(count);
    for (size_t cluster = 0; cluster < count; ++cluster)
        sorted[write[partition[cluster]]++] = m_Clusters[cluster];
    m_Clusters = std::move(sorted);
    return offsets;
}

// Locks every position that more than one group of this level uses, then expands the position
// locks to the vertex locks meshoptimizer reads.
void DagBuilder::LockGroupBorders(std::span<const uint32> offsets)
{
    m_PositionLocks = m_SourceLocks;
    Vector<uint32> owner(m_Mesh.Vertices.size(), kMeshClusterLODUnusedVertex);
    for (uint32 group = 0; group + 1 < offsets.size(); ++group)
    {
        for (uint32 cluster = offsets[group]; cluster < offsets[group + 1]; ++cluster)
        {
            const Cluster& source = m_Clusters[cluster];
            for (size_t corner = 0; corner < source.IndexCount; ++corner)
            {
                const uint32 position = m_Positions[m_ClusterIndices[source.IndexOffset + corner]];
                if (owner[position] != kMeshClusterLODUnusedVertex && owner[position] != group)
                    m_PositionLocks[position] |= meshopt_SimplifyVertex_Lock;
                owner[position] = group;
            }
        }
    }
    for (size_t vertex = 0; vertex < m_VertexLocks.size(); ++vertex)
        m_VertexLocks[vertex] = m_PositionLocks[m_Positions[vertex]];
}

// True when no triangle mixes tangent handedness: a normal map would shade it wrong.
bool KeepsTangentHandedness(const Mesh& mesh, std::span<const uint32> indices)
{
    for (size_t offset = 0; offset < indices.size(); offset += 3)
    {
        const float handedness = mesh.Vertices[indices[offset]].Tangent[3];
        if (mesh.Vertices[indices[offset + 1]].Tangent[3] != handedness ||
            mesh.Vertices[indices[offset + 2]].Tangent[3] != handedness)
            return false;
    }
    return true;
}

// The result has positive-area triangles that keep their tangent handedness, and the border
// edges of the group: no seam and no group border opens, and every non-manifold edge stays.
bool DagBuilder::IsValidReduction(std::span<const uint32> merged, std::span<const uint32> simplified) const
{
    return !simplified.empty() && simplified.size() < merged.size() && AllTrianglesHaveArea(m_Mesh, simplified) &&
        (!KeepsTangentHandedness(m_Mesh, merged) || KeepsTangentHandedness(m_Mesh, simplified)) &&
        BorderEdges(simplified, m_Positions) == BorderEdges(merged, m_Positions);
}

// Clusters vertices without topology limits. Accepts the result only when it keeps the group's
// border edges, so the locked group borders stay watertight.
bool DagBuilder::SimplifySloppy(std::span<const uint32> merged, size_t target, Vector<uint32>& output,
                                float& error) const
{
    Vector<uint32> local(m_Mesh.Vertices.size(), kMeshClusterLODUnusedVertex);
    Vector<uint32> vertices;
    Vector<Mathematics::Vector3> positions;
    Vector<uint8> fixed;
    Vector<uint32> indices;
    indices.reserve(merged.size());
    for (const uint32 index : merged)
    {
        if (local[index] == kMeshClusterLODUnusedVertex)
        {
            local[index] = static_cast<uint32>(vertices.size());
            vertices.push_back(index);
            const auto& position = m_Mesh.Vertices[index].Position;
            positions.push_back({position[0], position[1], position[2]});
            fixed.push_back((m_VertexLocks[index] & meshopt_SimplifyVertex_Lock) != 0 ? 1 : 0);
        }
        indices.push_back(local[index]);
    }
    const float scale = meshopt_simplifyScale(&positions.front().x, positions.size(), sizeof(Mathematics::Vector3));
    if (!(scale > 0.0f))
        return false;
    Vector<uint32> sloppy(indices.size());
    float relativeError = 0.0f;
    const size_t count = meshopt_simplifySloppy(sloppy.data(), indices.data(), indices.size(), &positions.front().x,
                                                positions.size(), sizeof(Mathematics::Vector3), fixed.data(), target,
                                                std::numeric_limits<float>::max(), &relativeError);
    sloppy.resize(count);
    for (uint32& index : sloppy)
        index = vertices[index];
    RemovePositionDegenerates(sloppy, m_Positions);
    error = relativeError * kSloppyErrorFactor * scale;
    if (!std::isfinite(error) || !IsValidReduction(merged, sloppy))
        return false;
    output = std::move(sloppy);
    return true;
}

// Simplifies one group to about half. False when the group does not reduce enough: then it is
// terminal and keeps its clusters.
bool DagBuilder::Simplify(std::span<const uint32> merged, Vector<uint32>& output, float& error) const
{
    const size_t target = std::max<size_t>(3, (static_cast<size_t>(merged.size() / 3 * kSimplifyRatio)) * 3);
    // The error is in mesh units, so every group of the mesh shares one scale.
    constexpr uint32 kOptions = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive |
                                meshopt_SimplifyErrorClamped;
    output.resize(merged.size());
    error = 0.0f;
    output.resize(meshopt_simplifyWithAttributes(
        output.data(), merged.data(), merged.size(), Positions(), m_Mesh.Vertices.size(), sizeof(Vertex),
        m_Attributes.data(), kAttributeCount * sizeof(float), kAttributeWeights, kAttributeCount, m_VertexLocks.data(),
        target, std::numeric_limits<float>::max(), kOptions, &error));
    RemovePositionDegenerates(output, m_Positions);
    // Attribute-aware simplification can retain three distinct but collinear vertices. Deleting
    // only that triangle could break the locked shared border, so such a result is not used.
    if (!std::isfinite(error) || error < 0.0f || !IsValidReduction(merged, output))
        output.clear();
    if (output.empty() || output.size() > target)
    {
        Vector<uint32> sloppy;
        float sloppyError = 0.0f;
        if (SimplifySloppy(merged, target, sloppy, sloppyError) && (output.empty() || sloppy.size() < output.size()))
        {
            output = std::move(sloppy);
            error = sloppyError;
        }
    }
    return !output.empty() && output.size() <= static_cast<size_t>(merged.size() * kStuckRatio);
}

// Appends one group and its units: the clusters of one producer join one unit.
bool DagBuilder::EmitGroup(std::span<const Cluster> clusters, std::span<const uint32> clusterIndices,
                           const ClusterBounds& bounds, uint32 depth, bool terminal, String& error)
{
    if (m_Dag.Groups.size() >= m_Config.MaximumGroups)
    {
        error = "Cluster DAG exceeds the group limit";
        return false;
    }
    const auto group = static_cast<uint32>(m_Dag.Groups.size());
    m_Dag.Groups.push_back({bounds.Center, bounds.Radius, bounds.Error, depth, terminal});
    Vector<const Cluster*> ordered;
    ordered.reserve(clusters.size());
    for (const Cluster& cluster : clusters)
        ordered.push_back(&cluster);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Cluster* a, const Cluster* b) { return a->Producer < b->Producer; });
    for (const Cluster* cluster : ordered)
    {
        if (m_Dag.Units.empty() || m_Dag.Units.back().Group != group || m_Dag.Units.back().Producer != cluster->Producer)
            m_Dag.Units.push_back({group, cluster->Producer, {}});
        auto& indices = m_Dag.Units.back().Indices;
        indices.insert(indices.end(), clusterIndices.begin() + cluster->IndexOffset,
                       clusterIndices.begin() + cluster->IndexOffset + cluster->IndexCount);
        m_UnitIndices += cluster->IndexCount;
    }
    const uint64 bytes = m_Mesh.Vertices.size() * sizeof(Vertex) + m_UnitIndices * sizeof(uint32);
    const uint64 metadata = m_Dag.Groups.size() * sizeof(MeshClusterLODDagGroup) +
        m_Dag.Units.size() * sizeof(MeshClusterLODDagUnit);
    if (bytes > m_Config.MaximumGeometryBytes || metadata > m_Config.MaximumMetadataBytes)
    {
        error = "Cluster DAG exceeds the geometry or metadata limit";
        return false;
    }
    return true;
}

bool DagBuilder::Build(MeshClusterLODDag& dag, String& error)
{
    if (!AdmitScratch(error))
        return false;
    LockSourcePositions(m_Mesh, m_Positions, m_SourceLocks);
    m_VertexLocks.resize(m_Mesh.Vertices.size());
    m_Attributes.reserve(m_Mesh.Vertices.size() * kAttributeCount);
    for (const Vertex& vertex : m_Mesh.Vertices)
    {
        m_Attributes.insert(m_Attributes.end(), std::begin(vertex.Normal), std::end(vertex.Normal));
        m_Attributes.insert(m_Attributes.end(), std::begin(vertex.TexCoords), std::end(vertex.TexCoords));
    }
    Clusterize(m_Mesh.Indices, kMeshClusterLODSourceGroup, nullptr);
    Vector<uint32> merged;
    Vector<uint32> simplified;
    uint32 depth = 0;
    while (m_Clusters.size() > 1)
    {
        if (depth >= kMaximumDepth || IsAssetDecodeCancelled())
        {
            error = depth >= kMaximumDepth ? "Cluster DAG exceeds the level limit" : "Cluster generation cancelled";
            return false;
        }
        const Vector<uint32> offsets = Partition();
        LockGroupBorders(offsets);
        // This level's clusters; the simplified groups fill m_Clusters with the next level.
        const Vector<Cluster> levelClusters = std::exchange(m_Clusters, {});
        const Vector<uint32> levelIndices = std::exchange(m_ClusterIndices, {});
        for (uint32 group = 0; group + 1 < offsets.size(); ++group)
        {
            const std::span<const Cluster> members(levelClusters.data() + offsets[group], offsets[group + 1] - offsets[group]);
            merged.clear();
            Vector<float> centers;
            Vector<float> radii;
            float childError = 0.0f;
            for (const Cluster& cluster : members)
            {
                merged.insert(merged.end(), levelIndices.begin() + cluster.IndexOffset,
                              levelIndices.begin() + cluster.IndexOffset + cluster.IndexCount);
                centers.insert(centers.end(), {cluster.Bounds.Center.x, cluster.Bounds.Center.y, cluster.Bounds.Center.z});
                radii.push_back(cluster.Bounds.Radius);
                childError = std::max(childError, cluster.Bounds.Error);
            }
            const meshopt_Bounds sphere = meshopt_computeSphereBounds(centers.data(), members.size(), 3 * sizeof(float),
                                                                      radii.data(), sizeof(float));
            ClusterBounds bounds{{sphere.center[0], sphere.center[1], sphere.center[2]}, sphere.radius, childError};
            float simplifyError = 0.0f;
            const bool reduced = Simplify(merged, simplified, simplifyError);
            if (reduced)
                bounds.Error = std::max(childError, simplifyError);
            if (!EmitGroup(members, levelIndices, bounds, depth, !reduced, error))
                return false;
            if (reduced)
                Clusterize(simplified, static_cast<uint32>(m_Dag.Groups.size() - 1), &bounds);
        }
        ++depth;
    }
    if (m_Clusters.size() == 1)
    {
        if (!EmitGroup(m_Clusters, m_ClusterIndices, m_Clusters.front().Bounds, depth, true, error))
            return false;
    }
    if (IsAssetDecodeCancelled())
    {
        error = "Cluster generation cancelled";
        return false;
    }
    dag = std::move(m_Dag);
    return true;
}

#endif

} // namespace

bool IsMeshClusterLODUnitInLevel(uint32 unitGroupDepth, bool unitGroupTerminal, uint32 depth)
{
    return unitGroupTerminal ? unitGroupDepth <= depth : unitGroupDepth == depth;
}

void CopyMeshClusterLODPlacement(const Mesh& source, Mesh& destination)
{
    destination.Name = source.Name;
    destination.MaterialIndex = source.MaterialIndex;
    destination.SourceNodeIndex = source.SourceNodeIndex;
    destination.SourceNodeParentIndex = source.SourceNodeParentIndex;
    std::copy(std::begin(source.SourceNodeTransform), std::end(source.SourceNodeTransform),
              std::begin(destination.SourceNodeTransform));
    std::copy(std::begin(source.SourceNodeLocalTransform), std::end(source.SourceNodeLocalTransform),
              std::begin(destination.SourceNodeLocalTransform));
    destination.ExtraPlacements = source.ExtraPlacements;
}

Mesh CompactMeshClusterLODTriangles(const Mesh& source, std::span<const uint32> indices,
                                   std::span<uint32> remap, Vector<uint32>& sourceVertices)
{
    Mesh result;
    CopyMeshClusterLODPlacement(source, result);
    result.Indices.reserve(indices.size());
    sourceVertices.clear();
    sourceVertices.reserve(std::min(indices.size(), source.Vertices.size()));
    for (const uint32 index : indices)
    {
        if (remap[index] == kMeshClusterLODUnusedVertex)
        {
            remap[index] = static_cast<uint32>(sourceVertices.size());
            sourceVertices.push_back(index);
        }
    }
    result.Vertices.reserve(sourceVertices.size());
    Mathematics::AABB bounds = Mathematics::AABB::Empty();
    for (const uint32 index : sourceVertices)
    {
        result.Vertices.push_back(source.Vertices[index]);
        const auto& position = source.Vertices[index].Position;
        bounds.Expand({position[0], position[1], position[2]});
    }
    for (const uint32 index : indices)
        result.Indices.push_back(remap[index]);
    for (const uint32 index : sourceVertices)
        remap[index] = kMeshClusterLODUnusedVertex;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        result.MinBounds[axis] = bounds.min[axis];
        result.MaxBounds[axis] = bounds.max[axis];
    }
    return result;
}

bool GenerateMeshClusterLODDag(const Mesh& mesh, const MeshClusterLODConfig& config, MeshClusterLODDag& dag,
                               String& error)
{
    error.clear();
    if (!ValidateInput(mesh, config, error))
        return false;
    if (IsAssetDecodeCancelled())
    {
        error = "Cluster generation cancelled";
        return false;
    }
#if !defined(GE_HAVE_MESHOPTIMIZER)
    (void)dag;
    error = "Cluster generation requires meshoptimizer";
    return false;
#else
    DagBuilder builder(mesh, config);
    return builder.Build(dag, error);
#endif
}

} // namespace GameEngine
