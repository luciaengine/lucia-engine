#pragma once

#include "Assets/ClusterLOD/MeshClusterLODGenerator.h"

#include <filesystem>
#include <span>

namespace GameEngine
{

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
bool DecodeMeshClusterLODFinePage(std::span<const uint8> bytes, const MeshClusterLODManifest::Page& page,
                                 const Mesh& coarse, Mesh& output, String& error);

/// Editor stores are beside the model; packaged stores are under AssetRoot/.cluster.
std::filesystem::path ResolveMeshClusterLODStore(const std::filesystem::path& modelPath,
                                                const std::filesystem::path& assetRoot,
                                                const MeshClusterLODManifest& manifest);

} // namespace GameEngine
