// Cook an already staged runtime using the same metadata, keys and validation
// as BuildPipeline. No editor, window, graphics device or authoring-tree writes.
#include "Engine/Build/TexturePackageCook.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetDecodeGate.h"
#include "Assets/TextureCookWorkers.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    using namespace GameEngine;
    namespace fs = std::filesystem;
    if (argc != 2) {
        std::cerr << "Usage: TexturePackageCook <staged-runtime-root>\n";
        return 2;
    }
    try {
        const auto root = fs::canonical(argv[1]);
        AssetManifest manifest;
        const auto collect = [&](const fs::path& mount, const std::string& alias) {
            AssetDatabase::AssetStore_TextJsonl store;
            std::string error;
            const auto manifestPath = mount / "Assets/.assetmanifest";
            if (!fs::is_regular_file(manifestPath))
                throw std::runtime_error("Missing staged manifest: " + manifestPath.string());
            if (!store.LoadFromFile(manifestPath, &error))
                throw std::runtime_error(error);
            for (const auto& record : store.EnumerateAssets()) {
                if (record.missing || record.type != AssetType::Texture) continue;
                const fs::path relative(record.path);
                if (relative.is_absolute()) throw std::runtime_error("Absolute texture path in manifest");
                for (const auto& part : relative)
                    if (part == "..") throw std::runtime_error("Texture path escapes asset root");
                AssetManifestEntry entry;
                entry.guid = record.guid;
                entry.type = record.type;
                entry.sourcePath = mount / "Assets" / relative;
                entry.outputPath = entry.sourcePath.lexically_relative(root);
                entry.sourceAlias = alias;
                manifest.entries.push_back(std::move(entry));
            }
        };
        collect(root, "");
        if (fs::is_directory(root / "Packages"))
            for (const auto& package : fs::directory_iterator(root / "Packages"))
                if (package.is_directory() && fs::is_regular_file(package.path() / "Assets/.assetmanifest"))
                    collect(package.path(), package.path().filename().string());
        // No mounted source registry: staged artifacts are still reused and
        // validated by the shared cooker; no external authoring cache is touched.
        AssetRegistry registry;
        JobSystem::WorkStealingThreadPool pool{16};
        auto gate = std::make_shared<AssetDecodeGate>(16, 0, nullptr);
        TextureCookWorkers workers{pool, std::move(gate)};
        TexturePackageCookStats stats;
        std::string error;
        if (!StagePackagedTextureCooks(root, manifest, registry,
                &workers, {}, stats, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << "Cooked " << stats.Textures << " textures, " << stats.Artifacts
                  << " artifacts (" << stats.ReusedArtifacts << " reused), "
                  << stats.Bytes << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
