#pragma once

#include "ECS/ECS.h"

#include <string>

namespace GameEngine
{
class UIElement;
namespace ECS
{
class World;
} // namespace ECS
} // namespace GameEngine

namespace GameEngine::Editor
{

// Only one terrain draws in a scene (TerrainECS::FindDrawnTerrain). The text tells the author that
// `entity` is active but not drawn, and names the drawn terrain. Empty when `entity` draws, is
// disabled, has no terrain handle yet, or carries no Terrain.
std::string DescribeUndrawnTerrain(ECS::World& world, ECS::EntityHandle entity);

// Shows DescribeUndrawnTerrain as a warning notice under `parent` when the text is not empty.
void AddTerrainDrawnNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity);

} // namespace GameEngine::Editor
