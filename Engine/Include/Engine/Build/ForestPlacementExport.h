#pragma once

#include <filesystem>
#include <string>

namespace GameEngine
{

struct ForestStandConfig;
class ShipRules;

/// Copy a configured placement archive to the packaged Player's asset root.
/// Empty means success. A configured archive is required for scene startup.
std::string StageForestPlacementArchive(const ForestStandConfig& forest,
    const std::filesystem::path& projectAssets, const std::filesystem::path& stagedAssets, const ShipRules& rules);

} // namespace GameEngine
