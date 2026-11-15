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
constexpr uint32 kMaximumGroups = 4096;
constexpr uint32 kMaximumGroupVertices = 65535;
// Leave headroom for meshoptimizer's products of float coordinates and attributes.
constexpr float kMaximumComponent = 100000000.0f;

bool ValidComponents(std::span<const float> values)
{
    return std::all_of(values.begin(), values.end(), [](float value)
    { return std::isfinite(value) && std::abs(value) <= kMaximumComponent; });
}

bool ValidateInput(const Mesh& mesh, const MeshClusterLODConfig& config, String& error)
{
    if (config.MeshletVertices < 3 || config.MeshletVertices > 256 ||
        config.MeshletTriangles < 4 || config.MeshletTriangles > 512 ||
        config.MeshletTriangles % 4 != 0 || config.MeshletsPerGroup == 0 ||
        config.MeshletsPerGroup > 128 || config.MaximumGroups == 0 ||
        config.MaximumGroups > kMaximumGroups || config.MaximumGeometryBytes == 0 ||
        config.MaximumGeometryBytes > kMaximumGeometryBytes ||
        config.MaximumMetadataBytes == 0 || config.MaximumMetadataBytes > kMaximumMetadataBytes ||
        config.MaximumScratchBytes == 0 || config.MaximumScratchBytes > kMaximumScratchBytes ||
        !std::isfinite(config.CoarseRatio) || config.CoarseRatio <= 0.0f || config.CoarseRatio >= 1.0f ||
        !std::isfinite(config.RelativeError) || config.RelativeError < 0.0f || config.RelativeError > 1.0f)
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

bool FindPositionLocks(const Mesh& mesh, const MeshClusterLODConfig& config,
                       Vector<uint32>& positions, Vector<uint8>& locked, String& error)
{
    const uint64 scratchBytes = mesh.Vertices.size() * (2 * sizeof(uint32) + sizeof(uint8) +
                                                       sizeof(Mathematics::Vector3)) +
                                mesh.Indices.size() * (sizeof(SourceEdge) + sizeof(uint32));
    if (scratchBytes > config.MaximumScratchBytes)
    {
        error = "Cluster topology scratch exceeds the limit";
        return false;
    }
    positions.resize(mesh.Vertices.size());
    Vector<Mathematics::Vector3> normalized;
    normalized.reserve(mesh.Vertices.size());
    for (const Vertex& vertex : mesh.Vertices)
        normalized.push_back({vertex.Position[0] == 0.0f ? 0.0f : vertex.Position[0],
                              vertex.Position[1] == 0.0f ? 0.0f : vertex.Position[1],
                              vertex.Position[2] == 0.0f ? 0.0f : vertex.Position[2]});
    meshopt_generatePositionRemap(positions.data(), &normalized.front().x,
                                 normalized.size(), sizeof(Mathematics::Vector3));
    locked.assign(mesh.Vertices.size(), 0);
    Vector<uint32> firstVertex(mesh.Vertices.size(), kMeshClusterLODUnusedVertex);
    for (uint32 index = 0; index < mesh.Vertices.size(); ++index)
    {
        const uint32 position = positions[index];
        if (firstVertex[position] != kMeshClusterLODUnusedVertex)
            locked[position] = meshopt_SimplifyVertex_Lock;
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
    std::sort(edges.begin(), edges.end(), [](const SourceEdge& a, const SourceEdge& b)
    { return a.Key < b.Key; });
    for (size_t begin = 0; begin < edges.size();)
    {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end].Key == edges[begin].Key)
            ++end;
        if (end - begin > 2)
        {
            error = "Cluster source has a non-manifold edge";
            return false;
        }
        if (end - begin == 1)
        {
            locked[static_cast<uint32>(edges[begin].Key >> 32)] = meshopt_SimplifyVertex_Lock;
            locked[static_cast<uint32>(edges[begin].Key)] = meshopt_SimplifyVertex_Lock;
        }
        else
        {
            const SourceEdge& first = edges[begin];
            const SourceEdge& second = edges[begin + 1];
            const uint32 firstThird = first.LowCorner - first.LowCorner % 3 +
                3 - first.LowCorner % 3 - first.HighCorner % 3;
            const uint32 secondThird = second.LowCorner - second.LowCorner % 3 +
                3 - second.LowCorner % 3 - second.HighCorner % 3;
            if (positions[mesh.Indices[firstThird]] == positions[mesh.Indices[secondThird]])
            {
                error = "Cluster source has a duplicate face";
                return false;
            }
            const bool firstForward = first.HighCorner % 3 == (first.LowCorner % 3 + 1) % 3;
            const bool secondForward = second.HighCorner % 3 == (second.LowCorner % 3 + 1) % 3;
            if (firstForward == secondForward)
            {
                error = "Cluster source has inconsistent edge winding";
                return false;
            }
            JoinCorners(parents, first.LowCorner, second.LowCorner);
            JoinCorners(parents, first.HighCorner, second.HighCorner);
        }
        begin = end;
    }
    std::fill(firstVertex.begin(), firstVertex.end(), kMeshClusterLODUnusedVertex);
    for (uint32 corner = 0; corner < mesh.Indices.size(); ++corner)
    {
        const uint32 position = positions[mesh.Indices[corner]];
        const uint32 root = FindCornerRoot(parents, corner);
        if (firstVertex[position] != kMeshClusterLODUnusedVertex && firstVertex[position] != root)
        {
            error = "Cluster source has a non-manifold vertex";
            return false;
        }
        firstVertex[position] = root;
    }
    return true;
}

bool PartitionTriangles(const Mesh& mesh, const MeshClusterLODConfig& config,
                        Vector<Vector<uint32>>& indices, String& error)
{
    const size_t bound = meshopt_buildMeshletsBound(mesh.Indices.size(), config.MeshletVertices,
                                                   config.MeshletTriangles);
    // Include retained position locks, referenced vertices and growing group-index arrays.
    const uint64 scratchBytes = mesh.Vertices.size() * (sizeof(uint32) + sizeof(uint8)) +
        static_cast<uint64>(bound) * (sizeof(meshopt_Meshlet) +
            3ull * config.MeshletVertices * sizeof(uint32) + 3ull * config.MeshletTriangles +
            2 * sizeof(uint32)) + 2ull * mesh.Indices.size() * sizeof(uint32) +
        static_cast<uint64>(config.MaximumGroups) * (sizeof(Vector<uint32>) + sizeof(size_t));
    if (scratchBytes > config.MaximumScratchBytes)
    {
        error = "Cluster partition scratch exceeds the limit";
        return false;
    }
    Vector<meshopt_Meshlet> meshlets(bound);
    Vector<uint32> vertices(bound * config.MeshletVertices);
    Vector<uint8> triangles(bound * config.MeshletTriangles * 3);
    const size_t count = meshopt_buildMeshlets(meshlets.data(), vertices.data(), triangles.data(),
                                              mesh.Indices.data(), mesh.Indices.size(),
                                              mesh.Vertices.front().Position, mesh.Vertices.size(),
                                              sizeof(Vertex), config.MeshletVertices,
                                              config.MeshletTriangles, 0.0f);
    if (count == 0 || IsAssetDecodeCancelled())
    {
        error = "Cluster partition was empty or cancelled";
        return false;
    }
    Vector<uint32> referenced;
    Vector<uint32> counts;
    size_t referencedCount = 0;
    for (size_t index = 0; index < count; ++index)
        referencedCount += meshlets[index].vertex_count;
    referenced.reserve(referencedCount);
    counts.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        const meshopt_Meshlet& meshlet = meshlets[index];
        referenced.insert(referenced.end(), vertices.begin() + meshlet.vertex_offset,
                          vertices.begin() + meshlet.vertex_offset + meshlet.vertex_count);
        counts.push_back(meshlet.vertex_count);
    }
    Vector<uint32> partition(count);
    const size_t groupCount = meshopt_partitionClusters(partition.data(), referenced.data(), referenced.size(),
                                                        counts.data(), count, mesh.Vertices.front().Position,
                                                        mesh.Vertices.size(), sizeof(Vertex),
                                                        config.MeshletsPerGroup);
    if (groupCount == 0 || groupCount > config.MaximumGroups)
    {
        error = "Cluster group count exceeds the limit";
        return false;
    }
    indices.resize(groupCount);
    Vector<size_t> groupSizes(groupCount, 0);
    for (size_t index = 0; index < count; ++index)
    {
        if (partition[index] >= groupCount)
        {
            error = "Cluster partition contains an invalid group";
            return false;
        }
        groupSizes[partition[index]] += meshlets[index].triangle_count * 3;
    }
    for (size_t group = 0; group < groupCount; ++group)
        indices[group].reserve(groupSizes[group]);
    for (size_t index = 0; index < count; ++index)
    {
        const meshopt_Meshlet& meshlet = meshlets[index];
        Vector<uint32>& group = indices[partition[index]];
        for (size_t corner = 0; corner < meshlet.triangle_count * 3; ++corner)
            group.push_back(vertices[meshlet.vertex_offset + triangles[meshlet.triangle_offset + corner]]);
    }
    if (std::any_of(indices.begin(), indices.end(), [](const Vector<uint32>& group) { return group.empty(); }))
    {
        error = "Cluster partition contains an empty group";
        return false;
    }
    return true;
}

bool AdmitSimplification(const Mesh& source, const Vector<Vector<uint32>>& indices,
                         const MeshClusterLODConfig& config, String& error)
{
    const uint64 retainedBytes = source.Vertices.size() * (3 * sizeof(uint32) + sizeof(uint8)) +
        2ull * source.Indices.size() * sizeof(uint32) + indices.capacity() * sizeof(Vector<uint32>);
    for (const Vector<uint32>& group : indices)
    {
        const uint64 vertices = std::min(group.size(), source.Vertices.size());
        const uint64 temporaryBytes = vertices * (2 * sizeof(uint32) + sizeof(uint8) + 5 * sizeof(float)) +
            group.size() * sizeof(uint32);
        if (retainedBytes + temporaryBytes > config.MaximumScratchBytes)
        {
            error = "Cluster simplification scratch exceeds the limit";
            return false;
        }
    }
    return true;
}

MeshClusterLODGroup SimplifyGroup(const Mesh& source, std::span<const uint32> indices,
                                 const Vector<uint32>& positions, const Vector<uint8>& positionLocks,
                                 const MeshClusterLODConfig& config, Vector<uint32>& remap)
{
    MeshClusterLODGroup result;
    Vector<uint32> sourceVertices;
    result.Fine = CompactMeshClusterLODTriangles(source, indices, remap, sourceVertices);
    Vector<uint8> locks;
    Vector<float> attributes;
    locks.reserve(sourceVertices.size());
    attributes.reserve(sourceVertices.size() * 5);
    for (const uint32 index : sourceVertices)
    {
        locks.push_back(positionLocks[positions[index]]);
        const Vertex& vertex = source.Vertices[index];
        attributes.insert(attributes.end(), std::begin(vertex.Normal), std::end(vertex.Normal));
        attributes.insert(attributes.end(), std::begin(vertex.TexCoords), std::end(vertex.TexCoords));
    }
    constexpr float kWeights[] = {2.0f, 2.0f, 2.0f, 0.5f, 0.5f};
    const size_t target = std::max<size_t>(3, (static_cast<size_t>(indices.size() * config.CoarseRatio) / 3) * 3);
    Vector<uint32> coarseIndices(indices.size());
    const size_t count = meshopt_simplifyWithAttributes(
        coarseIndices.data(), result.Fine.Indices.data(), result.Fine.Indices.size(),
        result.Fine.Vertices.front().Position, result.Fine.Vertices.size(), sizeof(Vertex),
        attributes.data(), 5 * sizeof(float), kWeights, 5, locks.data(), target,
        config.RelativeError, 0, &result.Error);
    if (count == 0 || count >= result.Fine.Indices.size() || !std::isfinite(result.Error))
    {
        result.Coarse = result.Fine;
        result.Error = 0.0f;
        return result;
    }
    result.Error *= meshopt_simplifyScale(result.Fine.Vertices.front().Position,
                                         result.Fine.Vertices.size(), sizeof(Vertex));
    if (!std::isfinite(result.Error) || result.Error < 0.0f)
    {
        result.Coarse = result.Fine;
        result.Error = 0.0f;
        return result;
    }
    coarseIndices.resize(count);
    // Attribute-aware simplification can retain three distinct but collinear
    // vertices. Preserve the whole fine group in that case: deleting only the
    // triangle could break the locked shared boundary of a mixed cut.
    for (size_t offset = 0; offset < coarseIndices.size(); offset += 3)
    {
        const auto& first = result.Fine.Vertices[coarseIndices[offset]].Position;
        const auto& second = result.Fine.Vertices[coarseIndices[offset + 1]].Position;
        const auto& third = result.Fine.Vertices[coarseIndices[offset + 2]].Position;
        const Mathematics::Vector3 edgeA{second[0] - first[0], second[1] - first[1], second[2] - first[2]};
        const Mathematics::Vector3 edgeB{third[0] - first[0], third[1] - first[1], third[2] - first[2]};
        if (Mathematics::Vector3::Cross(edgeA, edgeB).LengthSquared() <= 0.0f)
        {
            result.Coarse = result.Fine;
            result.Error = 0.0f;
            return result;
        }
    }
    Vector<uint32> coarseRemap(result.Fine.Vertices.size(), kMeshClusterLODUnusedVertex);
    result.Coarse = CompactMeshClusterLODTriangles(result.Fine, coarseIndices, coarseRemap, sourceVertices);
    // Both representations use the complete fine envelope for culling and selection.
    std::copy(std::begin(result.Fine.MinBounds), std::end(result.Fine.MinBounds),
              std::begin(result.Coarse.MinBounds));
    std::copy(std::begin(result.Fine.MaxBounds), std::end(result.Fine.MaxBounds),
              std::begin(result.Coarse.MaxBounds));
    return result;
}

#endif

} // namespace

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

bool GenerateMeshClusterLODs(const Mesh& mesh, const MeshClusterLODConfig& config,
                            Vector<MeshClusterLODGroup>& groups, String& error)
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
    error = "Cluster generation requires meshoptimizer";
    return false;
#else
    Vector<uint32> positions;
    Vector<uint8> locked;
    if (!FindPositionLocks(mesh, config, positions, locked, error))
        return false;
    Vector<Vector<uint32>> indices;
    if (!PartitionTriangles(mesh, config, indices, error))
        return false;
    const uint64 metadataPerGroup = sizeof(MeshClusterLODGroup) +
        2ull * (mesh.Name.size() + mesh.ExtraPlacements.size() * sizeof(MeshPlacement));
    if (indices.size() > config.MaximumMetadataBytes / metadataPerGroup ||
        indices.size() * (1 + mesh.ExtraPlacements.size()) > config.MaximumGroups)
    {
        error = "Cluster placement metadata exceeds the limit";
        return false;
    }
    if (!AdmitSimplification(mesh, indices, config, error))
        return false;
    Vector<uint32> firstGroup(mesh.Vertices.size(), kMeshClusterLODUnusedVertex);
    for (uint32 group = 0; group < indices.size(); ++group)
    {
        for (const uint32 index : indices[group])
        {
            const uint32 position = positions[index];
            if (firstGroup[position] != kMeshClusterLODUnusedVertex && firstGroup[position] != group)
                locked[position] = meshopt_SimplifyVertex_Lock;
            firstGroup[position] = group;
        }
    }
    Vector<MeshClusterLODGroup> generated;
    generated.reserve(indices.size());
    Vector<uint32> remap(mesh.Vertices.size(), kMeshClusterLODUnusedVertex);
    uint64 bytes = 0;
    for (const Vector<uint32>& group : indices)
    {
        if (IsAssetDecodeCancelled())
        {
            error = "Cluster generation cancelled";
            return false;
        }
        uint64 uniqueVertices = 0;
        for (const uint32 index : group)
        {
            if (remap[index] == kMeshClusterLODUnusedVertex)
            {
                remap[index] = 0;
                ++uniqueVertices;
            }
        }
        for (const uint32 index : group)
            remap[index] = kMeshClusterLODUnusedVertex;
        const uint64 maximumGroupBytes = 2 * (uniqueVertices * sizeof(Vertex) + group.size() * sizeof(uint32));
        if (uniqueVertices > kMaximumGroupVertices ||
            maximumGroupBytes > config.MaximumGeometryBytes - bytes)
        {
            error = "Cluster group exceeds the remaining geometry limit";
            return false;
        }
        MeshClusterLODGroup result = SimplifyGroup(mesh, group, positions, locked, config, remap);
        bytes += (result.Fine.Vertices.size() + result.Coarse.Vertices.size()) * sizeof(Vertex) +
                 (result.Fine.Indices.size() + result.Coarse.Indices.size()) * sizeof(uint32);
        if (result.Fine.Vertices.size() > kMaximumGroupVertices || bytes > config.MaximumGeometryBytes)
        {
            error = "Cluster output exceeds the geometry limit";
            return false;
        }
        generated.push_back(std::move(result));
    }
    if (IsAssetDecodeCancelled())
    {
        error = "Cluster generation cancelled";
        return false;
    }
    groups = std::move(generated);
    return true;
#endif
}

} // namespace GameEngine
