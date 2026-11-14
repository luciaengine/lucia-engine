#pragma once

#include "Engine/Rendering/RenderSortKey.h"

#include <cstddef>
#include <limits>
#include <optional>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Rendering { struct SortingLayers; }
namespace GameEngine::Engine::Renderer
{
/// Conservatively detects authored groups, including disabled rows.
bool HasRenderSortingGroups(ECS::World& world,
    std::size_t scanBudget = std::numeric_limits<std::size_t>::max());

/// Lazily shares a conservative group decision within one extraction update.
RenderSortingData ResolveRenderSortingForBatch(ECS::World& world, ECS::EntityHandle entity,
    const Rendering::SortingLayers& layers, std::optional<bool>& hasGroups, std::size_t scanBudget);

/// Resolves enabled sorting and outermost group membership, independently of the active view.
RenderSortingData ResolveRenderSorting(ECS::World& world, ECS::EntityHandle entity,
                                       const Rendering::SortingLayers& layers, bool hasGroups = true);
}
