// Offline owned source model: cooking never borrows a live editor asset.
#include "Assets/AssetManager.h"
#include "Assets/ClusterLOD/MeshClusterLODAsset.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
    using namespace GameEngine;
    namespace fs = std::filesystem;
    try
    {
        fs::path input, output, assets;
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            fs::path* destination = argument == "--input" ? &input :
                argument == "--output" ? &output : argument == "--assets" ? &assets : nullptr;
            if (!destination || !destination->empty() || i + 1 >= argc)
                throw std::runtime_error("Usage: MeshClusterCook --input MODEL --output NEW.gecluster [--assets ASSET_ROOT]");
            *destination = argv[++i];
        }
        if (input.empty() || output.empty())
            throw std::runtime_error("Input model and new output file are required");
        input = fs::canonical(input);
        output = fs::absolute(output).lexically_normal();
        if (ModelAsset::FormatFromPath(output) != ModelFormat::ClusterLOD ||
            fs::exists(output) || fs::is_symlink(output) || !fs::is_directory(output.parent_path()))
            throw std::runtime_error("Output must be a new .gecluster file inside an existing directory");

        // Only a published identity manifest may supply external texture GUIDs.
        // This immutable mount performs no scan, watcher, metadata write or cook.
        AssetManager manager;
        if (!manager.Initialize())
            throw std::runtime_error("Could not initialize asset parsers");
        if (!assets.empty())
        {
            assets = fs::canonical(assets);
            if (!fs::is_regular_file(assets / ".assetmanifest") ||
                !manager.RegisterSource(MakePackageMount("project", assets)))
                throw std::runtime_error("Asset root requires a valid published .assetmanifest");
        }
        AssetManager::ScopedThreadAssetManager current(&manager);
        ModelAsset source(GUID::Generate(), input);
        // Do not PostLoad: ordinary automatic LOD generation is not an input to
        // this explicit cluster cook. The cooker validates supported streams.
        if (!source.Load())
            throw std::runtime_error("Could not load the source model");
        String error;
        if (!CookMeshClusterLODAsset(source, output, MeshClusterLODConfig{}, error))
            throw std::runtime_error(error);

        ModelAsset generated(GUID::Generate(), output);
        if (!generated.Load() || !generated.GetClusterLODManifest())
            throw std::runtime_error("Generated coarse model failed verification");
        const auto& manifest = *generated.GetClusterLODManifest();
        constexpr uint64 kIndicesPerTriangle = 3;
        uint64 streamedIndexCount = 0;
        uint64 streamedUnitCount = 0;
        for (const MeshClusterLODManifest::Unit& unit : manifest.Units)
        {
            streamedIndexCount += unit.IsStreamed() ? unit.IndexCount : 0;
            streamedUnitCount += unit.IsStreamed() ? 1 : 0;
        }
        std::cout << "Coarse meshes: " << generated.GetMeshCount() << ", DAG groups: " << manifest.Groups.size()
                  << ", units: " << manifest.Units.size() << " (" << streamedUnitCount << " streamed)"
                  << ", coarse bytes: " << fs::file_size(output) << ", fine bytes: " << manifest.StoreBytes << '\n';
        for (uint32 meshIndex = 0; meshIndex < generated.GetMeshCount(); ++meshIndex)
        {
            const Mesh& mesh = generated.GetMesh(meshIndex);
            std::cout << "Mesh " << meshIndex << " resident level " << manifest.Meshes[meshIndex].ResidentDepth
                      << ", LOD0 triangles: " << mesh.Indices.size() / kIndicesPerTriangle << '\n';
        }
        std::cout << "Streamed triangles: " << streamedIndexCount / kIndicesPerTriangle << '\n';
        std::cout << "Vertices (imported/coarse): " << source.GetTotalVertexCount() << '/'
                  << generated.GetTotalVertexCount() << '\n';
        std::cout << "Fine store: "
                  << ResolveMeshClusterLODStore(output, {}, manifest).generic_string() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
