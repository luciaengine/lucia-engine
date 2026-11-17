#pragma once

#include "Assets/ClusterLOD/MeshClusterLODGenerator.h"
#include "Mathematics/Vector3.h"

#include <filesystem>
#include <span>

namespace GameEngine
{

/// The cluster DAG of every coarse mesh under its resident level. A coarse mesh is the resident
/// cut of its DAG (LOD0); its groups are terminal, so a split never draws coarser than LOD0. The
/// far chain is the ordinary generated LOD chain over LOD0. Finer units stream as immutable fine
/// pages.
struct MeshClusterLODManifest
{
    static constexpr uint64 kMaximumFileBytes = 256ull * 1024ull * 1024ull;
    struct Group
    {
        // The sphere encloses the spheres of every group under it, in mesh-local units.
        Mathematics::Vector3 Center;
        float Radius = 0.0f;
        // The error of the group's simplified units in mesh-local units: at least the error of
        // every group under it. Not used for a terminal group.
        float Error = 0.0f;
        uint32 Mesh = 0;
        uint32 Depth = 0;
        // A terminal group did not simplify and is always refined.
        bool Terminal = false;
        // The units of this group: Units[FirstUnit, FirstUnit + UnitCount).
        uint32 FirstUnit = 0;
        uint32 UnitCount = 0;
        // The units this group produced: ProducedUnits[FirstProduced, FirstProduced + ProducedCount).
        uint32 FirstProduced = 0;
        uint32 ProducedCount = 0;
    };
    struct Unit
    {
        uint32 Mesh = 0;
        // Manifest group indices. A unit draws when Group is refined and Producer is not.
        uint32 Group = 0;
        uint32 Producer = kMeshClusterLODSourceGroup;
        uint32 IndexCount = 0;
        // Mesh-local bounds of the unit's vertices.
        Mathematics::Vector3 MinBounds;
        Mathematics::Vector3 MaxBounds;
        // A resident unit is the range [FirstIndex, FirstIndex + IndexCount) of LOD0 of its coarse mesh.
        uint32 FirstIndex = 0;
        // A streamed unit is one page in the fine store: the meshopt-encoded vertex stream, then
        // the meshopt-encoded index stream. Bytes and ContentHash cover both streams.
        uint64 Offset = 0;
        uint64 Bytes = 0;
        uint64 ContentHash = 0;
        uint32 EncodedVertexBytes = 0;
        uint32 VertexCount = 0;

        bool IsStreamed() const { return Bytes != 0; }
    };
    struct MeshRange
    {
        uint32 FirstGroup = 0;
        uint32 GroupCount = 0;
        uint32 FirstUnit = 0;
        uint32 UnitCount = 0;
        // The DAG level of LOD0.
        uint32 ResidentDepth = 0;
    };

    uint64 Incarnation = 0;
    uint64 StoreHash = 0;
    uint64 StoreBytes = 0;
    Vector<Group> Groups;
    // Sorted by mesh, then group, then producer.
    Vector<Unit> Units;
    Vector<uint32> ProducedUnits;
    Vector<MeshRange> Meshes;
    Vector<GUID> TextureDependencies;
};

struct MeshClusterLODAssetData
{
    MeshClusterLODManifest Manifest;
    Vector<Mesh> CoarseMeshes;
    Vector<ImportedMaterialData> Materials;
    Vector<EmbeddedImage> Images;
};

/// Writes immutable fine pages, then atomically publishes the coarse model.
/// Static hierarchy metadata is permitted; animation and deforming streams are refused.
/// External textures require registered asset GUIDs. Source data is unchanged.
bool CookMeshClusterLODAsset(const ModelAsset& source, const std::filesystem::path& output,
                            const MeshClusterLODConfig& config, String& error);

/// Decodes the coarse model and the DAG without opening its fine store.
bool DecodeMeshClusterLODAsset(std::span<const uint8> bytes, MeshClusterLODAssetData& output, String& error);

/// Validates one immutable page before publishing the complete mesh of a streamed unit.
/// The output takes the placement and material of `coarse`, the unit's coarse mesh.
bool DecodeMeshClusterLODFinePage(std::span<const uint8> bytes, const MeshClusterLODManifest::Unit& unit,
                                 const Mesh& coarse, Mesh& output, String& error);

/// A resident unit as a standalone mesh: its index range of `coarse`, compacted to the vertices
/// it uses. Placement and material come from `coarse`.
Mesh ExtractMeshClusterLODResidentUnit(const Mesh& coarse, const MeshClusterLODManifest::Unit& unit);

/// The largest error of the units of LOD0 of one coarse mesh, relative to its largest bounds
/// axis: the unit of Mesh::ExtraLODErrors. Zero for a mesh without streamed units.
float MeshClusterLODRelativeCoarseError(const MeshClusterLODManifest& manifest, uint32 mesh, const Mesh& coarse);

/// Editor stores are beside the model; packaged stores are under AssetRoot/.cluster.
std::filesystem::path ResolveMeshClusterLODStore(const std::filesystem::path& modelPath,
                                                const std::filesystem::path& assetRoot,
                                                const MeshClusterLODManifest& manifest);

} // namespace GameEngine
