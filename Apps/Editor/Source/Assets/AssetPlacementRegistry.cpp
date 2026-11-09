#include "Assets/AssetPlacementRegistry.h"

namespace GameEngine::Editor
{

AssetPlacementRegistry& AssetPlacementRegistry::Get()
{
    static AssetPlacementRegistry s_Registry;
    return s_Registry;
}

void AssetPlacementRegistry::Register(AssetType type, PlaceFunction place)
{
    m_Handlers.InsertOrAssign(type, place);
}

AssetPlacementRegistry::PlaceFunction AssetPlacementRegistry::Find(AssetType type) const
{
    const PlaceFunction* handler = m_Handlers.Find(type);
    return handler ? *handler : nullptr;
}

} // namespace GameEngine::Editor
