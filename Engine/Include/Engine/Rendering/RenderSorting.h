#pragma once

#include "Engine/Rendering/RenderSortKey.h"

namespace GameEngine::ECS { class World; }
namespace GameEngine::Rendering { struct SortingLayers; }
namespace GameEngine::Engine::Renderer
{
/// Resolves enabled sorting and outermost group membership, independently of the active view.
RenderSortingData ResolveRenderSorting(ECS::World& world, ECS::EntityHandle entity,
                                       const Rendering::SortingLayers& layers, bool hasGroups = true);
}
