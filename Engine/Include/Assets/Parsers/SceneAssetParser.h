#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/SceneAsset.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace GameEngine
{

class SceneAssetParser final : public AssetParser
{
  public:
    explicit SceneAssetParser(AssetType type = AssetType::Scene) : m_Type(type) {}

    AssetType GetAssetType() const override { return m_Type; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        if (m_Type == AssetType::Prefab)
            return {".prefab"};
        return {".scene", ".blueprint"};
    }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<SceneAsset>(metadata.Guid, metadata.Path, m_Type);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create Scene asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "SceneAssetParser"; }

    int GetPriority() const override { return 30; }

    /// Collect component-field and resource dependencies from scene and prefab text.
    /// Locators distinguish entities, prefab instances and resources; GUID and path forms stay separate.
    /// Prefab text uses PrefabComponent edges for both field references and resource headers.
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;

    /// Collect names of components assigned under entity or prefab instance sections.
    static void CollectComponentNames(std::string_view body, std::unordered_set<std::string>& outNames);

    /// Remove editor-only component operations from scene and prefab text.
    /// For markup entities, also remove Name and Spline data; preserve headers and other lines.
    static std::string StripEditorOnlyComponents(std::string_view body,
                                                 const std::function<bool(std::string_view)>& isEditorOnly);

  private:
    AssetType m_Type;
};

} // namespace GameEngine
