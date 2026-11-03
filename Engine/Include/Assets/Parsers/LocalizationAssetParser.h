#pragma once

#include "Assets/ParserRegistry.h"
#include "Localization/LocalizationAsset.h"

namespace GameEngine
{
class LocalizationAssetParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::Localization; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".localization"}; }
    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager&) override
    {
        return AssetParseResult(std::make_shared<Localization::LocalizationAsset>(metadata.Guid, metadata.Path));
    }
    std::string GetName() const override { return "LocalizationAssetParser"; }
};
}
