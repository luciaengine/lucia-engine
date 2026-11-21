#include "Assets/HlodBakeDriver.h"

#include "Assets/AssetManager.h"
#include "Assets/HlodCache.h"
#include "Assets/HlodClustering.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/HLODVolume.h"

#include "ECS/Components.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"

#include <optional>
#include <system_error>
#include <string>

namespace GameEngine {
namespace Hlod {

std::filesystem::path ResolveHlodCacheFile(AssetManager& assetManager,
                                           const std::filesystem::path& scenePath) {
    const std::filesystem::path assetRoot = assetManager.GetAssetRoot();
    const auto& registry = assetManager.GetRegistry();
    const GUID scene = registry.GetAssetGUID(scenePath);
    const std::string key = scene.IsNull() ? scenePath.stem().string() : scene.ToString();
    if (const auto cacheRoot = registry.TryGetCacheRoot(scenePath))
        return *cacheRoot / "Hlod" / (key + ".gehlod");
    if (!assetRoot.empty())
        return assetRoot / ".hlod" / (key + ".gehlod");
    return {};
}

std::filesystem::path ResolveExistingHlodCacheFile(AssetManager& assetManager,
                                                  const std::filesystem::path& scenePath)
{
    const auto path = ResolveHlodCacheFile(assetManager, scenePath);
    std::error_code error;
    if (path.empty() || std::filesystem::is_regular_file(path, error))
        return path;
    return path.parent_path() / (scenePath.stem().string() + ".gehlod");
}

std::filesystem::path PackagedHlodCacheFile(const std::filesystem::path& assetRoot, const GUID& scene)
{
    if (assetRoot.empty() || scene.IsNull())
        return {};
    return assetRoot / ".hlod" / (scene.ToString() + ".gehlod");
}

namespace {

constexpr uint64 kBytesPerMegabyte = 1024ull * 1024ull;

} // namespace

bool ResolveHlodVolumeConfig(GameEngine::ECS::World& world, GridConfig& outConfig) {
    bool found = false;
    world.Query<GameEngine::ECS::Read<GameEngine::Components::HLODVolume>>().Each(
        [&](const GameEngine::Components::HLODVolume& volume) {
            if (found)
                return;
            found = true;
            outConfig.CellSize = volume.CellSize;
            outConfig.MaxInstancingRatio = volume.MaxInstancingRatio;
            outConfig.MinMembers = volume.MinMembers;
            outConfig.VbBudgetBytes = static_cast<uint64>(volume.VBBudgetMB) * kBytesPerMegabyte;
        });
    return found;
}

BakeDriverResult BakeHlodForWorld(GameEngine::ECS::World& world,
                                  const ModelResolver& resolveModel,
                                  const std::filesystem::path& outPath) {
    BakeDriverResult result;

    GridConfig config;
    if (!ResolveHlodVolumeConfig(world, config)) {
        result.Outcome = BakeOutcome::NoVolume;
        return result;
    }

    GatheredMembers gathered = GatherHlodMembers(world, resolveModel);
    result.GatheredMembers = static_cast<uint32>(gathered.Members.size());
    result.SkippedMembers = gathered.SkippedSkinned + gathered.SkippedMorph +
                            gathered.SkippedNoModel + gathered.SkippedNoMaterial +
                            gathered.SkippedNonTriangle + gathered.SkippedExtraVertexStreams + gathered.SkippedDisabled +
                            gathered.SkippedShadowsOnly;
    if (gathered.Members.empty()) {
        result.Outcome = BakeOutcome::NoMembers;
        return result;
    }

    HlodBakedScene scene =
        BakeScene(gathered.Members, gathered.Geometry, config, &result.Stats);

    if (!WriteHlodCache(outPath, scene)) {
        result.Outcome = BakeOutcome::WriteFailed;
        return result;
    }

    result.Outcome = BakeOutcome::Wrote;
    return result;
}

bool BakeHlodForScene(AssetManager& assets, GameEngine::ECS::World& world,
                      const std::filesystem::path& scenePath, std::string* outError) {
    const std::filesystem::path outPath = ResolveHlodCacheFile(assets, scenePath);
    if (outPath.empty()) {
        if (outError)
            *outError = "no cache root for scene";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);

    const ModelResolver resolver = [&assets](const GUID& guid) -> SharedPtr<const ModelAsset> {
        return std::dynamic_pointer_cast<const ModelAsset>(assets.GetAsset(guid));
    };

    const BakeDriverResult result = BakeHlodForWorld(world, resolver, outPath);
    switch (result.Outcome) {
    case BakeOutcome::Wrote:
        Logger::Log::Info(
            "Bake HLOD: wrote '{}' ({} clusters, {} admitted, {} gathered / {} skipped members, "
            "{} KB proxy VB)",
            outPath.string(), result.Stats.ClusterCount, result.Stats.AdmittedCount,
            result.GatheredMembers, result.SkippedMembers, result.Stats.ProxyVertexBytes / 1024ull);
        return true;
    case BakeOutcome::NoVolume:
        if (outError)
            *outError = "no enabled HLODVolume in the scene";
        return false;
    case BakeOutcome::NoMembers:
        if (outError)
            *outError = "no eligible static members gathered";
        return false;
    case BakeOutcome::WriteFailed:
        if (outError)
            *outError = "atomic write of .gehlod failed";
        return false;
    }
    return false;
}

} // namespace Hlod
} // namespace GameEngine
