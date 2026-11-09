#pragma once

#include "Assets/RuntimeAssetMetadata.h"
#include "Types/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{

class AssetRegistry;

/// One model's reduced skeleton tier. Mandatory paths are arrays of node-name
/// segments from root to leaf, so names containing separators remain unambiguous.
struct BoneLODAssetSettings
{
    uint32 BoneBudget = 32u;
    std::vector<std::vector<std::string>> MandatoryPaths;
    // A malformed setting disables reduction; required bones must not be discarded.
    std::string Error;

    static constexpr const char* kMetadataKey = kBoneLODSettingsMetaKey;

    static BoneLODAssetSettings Load(const AssetRegistry& registry, const std::filesystem::path& assetPath);
    /// Encodes one atomic metadata edit for the editor's undo history.
    std::optional<std::string> Encode() const;
    bool Save(AssetRegistry& registry, const std::filesystem::path& assetPath) const;
};

} // namespace GameEngine
