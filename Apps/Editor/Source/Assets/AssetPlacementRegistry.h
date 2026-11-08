#pragma once

#include "AssetCore/AssetTypes.h"
#include "ECS/ECS.h"
#include "Types/FlatMap.h"

#include <filesystem>

namespace GameEngine
{
class AssetManager;
namespace Components
{
struct Transform;
}
}

namespace GameEngine::Editor
{

/// Asset types that editor features can place in a world. Register on the main thread at startup.
class AssetPlacementRegistry
{
  public:
    using PlaceFunction = ECS::EntityHandle (*)(ECS::World&, AssetManager&, const std::filesystem::path&,
                                               const Components::Transform&, ECS::EntityHandle);

    static AssetPlacementRegistry& Get();
    /// Replaces the placement handler for an asset type.
    void Register(AssetType type, PlaceFunction place);
    /// Returns the registered handler, or null when the type has none.
    PlaceFunction Find(AssetType type) const;

  private:
    FlatMap<AssetType, PlaceFunction> m_Handlers;
};

} // namespace GameEngine::Editor
