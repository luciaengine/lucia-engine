#pragma once

#include "AssetCore/Asset.h"
#include <string>

namespace GameEngine
{

/// Scene and prefab text share one authoring grammar.
/// Scene::LoadSceneFromFile instantiates that text into an ECS World.
class SceneAsset final : public Asset
{
  public:
    SceneAsset(const GUID& guid, const std::filesystem::path& path, AssetType type)
        : Asset(guid, type, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const std::string& GetText() const { return m_Text; }

  private:
    std::string m_Text;
};

} // namespace GameEngine

