#pragma once

#include "Assets/ClusterLOD/MeshClusterLODGenerator.h"
#include "Mathematics/Vector3.h"

#include <filesystem>
#include <span>

namespace GameEngine
{

/// One coarse mesh per source mesh. Each cluster group is a contiguous index range of its
/// coarse mesh and has one immutable fine page that replaces that range.
struct MeshClusterLODManifest
{
    static constexpr uint64 kMaximumFileBytes = 256ull * 1024ull * 1024ull;
    struct Page
    {
        uint64 Offset = 0;
        uint64 Bytes = 0;
        uint64 ContentHash = 0;
        uint32 VertexCount = 0;
        uint32 IndexCount = 0;
        float Error = 0.0f;
        // The coarse cover: mesh index, then the index range in that mesh's LOD0 indices.
        uint32 Mesh = 0;
        uint32 CoarseFirstIndex = 0;
        uint32 CoarseIndexCount = 0;
        // Mesh-local bounds of the group's coarse and fine vertices.
        Mathematics::Vector3 MinBounds;
        Mathematics::Vector3 MaxBounds;
    };

    uint64 Incarnation = 0;
    uint64 StoreHash = 0;
    uint64 StoreBytes = 0;
    Vector<Page> Pages;
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

/// Decodes the coarse model and page directory without opening its fine store.
bool DecodeMeshClusterLODAsset(std::span<const uint8> bytes, MeshClusterLODAssetData& output, String& error);

/// Validates one immutable page before publishing its complete fine mesh.
/// The output takes the placement and material of `coarse`, the page's coarse mesh.
bool DecodeMeshClusterLODFinePage(std::span<const uint8> bytes, const MeshClusterLODManifest::Page& page,
                                 const Mesh& coarse, Mesh& output, String& error);

/// The coarse cover of one page as a standalone mesh: the page's index range of `coarse`,
/// compacted to the vertices it uses. Placement and material come from `coarse`.
Mesh ExtractMeshClusterLODCoarseGroup(const Mesh& coarse, const MeshClusterLODManifest::Page& page);

/// The largest page error of one coarse mesh, relative to its largest bounds axis: the unit of
/// Mesh::ExtraLODErrors. Zero for a mesh without pages.
float MeshClusterLODRelativeCoarseError(const MeshClusterLODManifest& manifest, uint32 mesh, const Mesh& coarse);

/// Editor stores are beside the model; packaged stores are under AssetRoot/.cluster.
std::filesystem::path ResolveMeshClusterLODStore(const std::filesystem::path& modelPath,
                                                const std::filesystem::path& assetRoot,
                                                const MeshClusterLODManifest& manifest);

} // namespace GameEngine
