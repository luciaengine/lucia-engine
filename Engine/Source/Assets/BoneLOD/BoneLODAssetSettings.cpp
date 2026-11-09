#include "Assets/BoneLOD/BoneLODAssetSettings.h"

#include "Assets/AssetRegistry.h"

#include <limits>

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

bool ReadSettings(const nlohmann::json& document, BoneLODAssetSettings& settings)
{
    if (!document.is_object())
        return false;
    for (const auto& [key, value] : document.items())
    {
        if (key == "budget")
        {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() && value.get<uint64>() > std::numeric_limits<uint32>::max()) ||
                (!value.is_number_unsigned() && (value.get<int64>() < 0 ||
                    value.get<int64>() > std::numeric_limits<uint32>::max())))
                return false;
            settings.BoneBudget = value.get<uint32>();
        }
        else if (key == "mandatoryPaths")
        {
            if (!value.is_array())
                return false;
            for (const auto& path : value)
            {
                if (!path.is_array() || path.empty())
                    return false;
                std::vector<std::string> segments;
                for (const auto& segment : path)
                {
                    if (!segment.is_string())
                        return false;
                    segments.push_back(segment.get<std::string>());
                }
                settings.MandatoryPaths.push_back(std::move(segments));
            }
        }
        else
        {
            return false;
        }
    }
    return true;
}

} // namespace

BoneLODAssetSettings BoneLODAssetSettings::Load(const AssetRegistry& registry,
    const std::filesystem::path& assetPath)
{
    BoneLODAssetSettings settings;
    std::string metadata;
    if (assetPath.empty() || !registry.TryGetMetaValue(assetPath, kMetadataKey, metadata) || metadata.empty())
        return settings;

    const auto document = nlohmann::json::parse(metadata, nullptr, false);
    if (document.is_discarded() || !ReadSettings(document, settings))
    {
        settings.BoneBudget = 0u;
        settings.MandatoryPaths.clear();
        settings.Error = "Bone LOD settings are invalid. Reset the model's bone LOD settings before reducing its skeleton.";
    }
    return settings;
}

std::optional<std::string> BoneLODAssetSettings::Encode() const
{
    if (!Error.empty())
        return std::nullopt;
    for (const auto& path : MandatoryPaths)
    {
        if (path.empty())
            return std::nullopt;
    }
    try
    {
        const nlohmann::json document = {{"budget", BoneBudget}, {"mandatoryPaths", MandatoryPaths}};
        return document.dump();
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }
}

bool BoneLODAssetSettings::Save(AssetRegistry& registry, const std::filesystem::path& assetPath) const
{
    const auto metadata = Encode();
    return !assetPath.empty() && metadata && registry.SetMetaValue(assetPath, kMetadataKey, *metadata);
}

} // namespace GameEngine
