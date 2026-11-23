#include "Engine/Build/StagedAssetCook.h"

#include "Assets/ParserRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Build/SceneExportStrip.h"
#include "Engine/Build/ShipRules.h"
#include "Engine/Build/TexturePackageCook.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Types/PathUtils.h"

#include <algorithm>

namespace GameEngine
{
namespace fs = std::filesystem;

namespace
{
bool IsAssetRelative(const fs::path& path)
{
    return !path.empty() && !path.has_root_path() &&
           std::ranges::none_of(path, [](const fs::path& part) { return part == ".."; });
}
} // namespace

std::string StageForestPlacementArchive(const ForestStandConfig& forest,
    const fs::path& projectAssets, const fs::path& stagedAssets, const ShipRules& rules)
{
    const fs::path relative = PathFromUtf8(forest.Archive);
    if (!IsAssetRelative(relative) || relative.extension() != ".geforest" ||
        !AssetCollector::IsShippableContent(relative))
        return "forestStand.archive must name a .geforest file under the project's Assets folder";

    const fs::path normalized = relative.lexically_normal();
    const std::string key = PathToUtf8(normalized);
    const auto rule = rules.Resolve(key);
    if (rule.Rule == ShipRule::Never)
        return ShipRules::DescribeNever(key, rule) + ", but forestStand.archive requires it. Change the ship rule or the archive.";

    std::error_code error;
    const fs::path root = fs::canonical(projectAssets, error);
    if (error)
        return "Could not open the project asset folder for forestStand.archive: " + error.message();
    const fs::path source = fs::canonical(root / normalized, error);
    if (error || !fs::is_regular_file(source, error))
        return "Could not open forestStand.archive '" + forest.Archive + "' in the project asset folder";
    const fs::path sourceRelative = source.lexically_relative(root);
    if (!IsAssetRelative(sourceRelative) || !AssetCollector::IsShippableContent(sourceRelative))
        return "forestStand.archive must point inside the project's shippable asset folders";
    const std::string resolvedKey = PathToUtf8(sourceRelative);
    const auto sourceRule = rules.Resolve(resolvedKey);
    if (sourceRule.Rule == ShipRule::Never)
        return ShipRules::DescribeNever(resolvedKey, sourceRule) + ", but forestStand.archive points to it.";

    PageStreaming::PackedPlacementStore archive;
    if (std::string reason = archive.Open(source); !reason.empty())
        return "Invalid forestStand.archive '" + forest.Archive + "': " + reason;
    const fs::path destination = stagedAssets / normalized;
    fs::create_directories(destination.parent_path(), error);
    if (!error)
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
    if (error)
        return "Could not stage forestStand.archive '" + forest.Archive + "': " + error.message();
    return {};
}

bool CookStagedContent(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                       const AssetRegistry& registry, const ParserRegistry& parsers,
                       TextureCookWorkers* textureWorkers,
                       JobSystem::WorkStealingThreadPool* manifestParsePool,
                       const std::function<bool()>& cancelRequested, TexturePackageCookStats& textureStats,
                       std::string& error)
{
    if (!StripEditorOnlyComponentsFromStagedScenes(contentRoot, manifest, cancelRequested, error))
        return false;
    if (!StagePackagedTextureCooks(contentRoot, manifest, registry, textureWorkers,
                                   manifestParsePool, cancelRequested, textureStats, error))
        return false;
    return CookStagedAssets(contentRoot, manifest, parsers, cancelRequested, error);
}

bool CookStagedAssets(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                      const ParserRegistry& parsers, const std::function<bool()>& cancelRequested,
                      std::string& error)
{
    error.clear();
    for (const auto& entry : manifest.entries)
    {
        if (cancelRequested && cancelRequested())
            return false;
        const auto parser = parsers.FindParser(entry.sourcePath);
        if (!parser)
            continue;
        std::string reason;
        const AssetPackageCookContext context{entry.sourcePath, contentRoot / entry.outputPath, contentRoot / "Assets"};
        if (!parser->CookForPackage(context, reason))
        {
            error = entry.outputPath.generic_string() + ": " + reason;
            return false;
        }
    }
    return true;
}
} // namespace GameEngine
