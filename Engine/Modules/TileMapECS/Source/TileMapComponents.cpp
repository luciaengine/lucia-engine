#include "Components/TileMap/TileMapCollider.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::TileMapLayerRef);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::TileMapCollider);
} // namespace GameEngine::ECS
