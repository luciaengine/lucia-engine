#pragma once

#include "ECS/ECS.h"

#include <cstdint>

namespace GameEngine::SplineRecipes
{

/// Where a spline recipe's build stands.
enum class BuildState : std::uint8_t
{
    /// The recipe has not built yet, or an edit to it waits for its rebuild.
    Pending = 0,
    /// The recipe waits for something it builds on, such as the terrain under it. ReasonOf says what.
    Held = 1,
    /// The recipe's output matches the recipe. When the output is empty, ReasonOf says why.
    Built = 2,
    /// The recipe cannot build. ReasonOf says why and how to fix it.
    Failed = 3,
};

/// A recipe entity and its build state.
struct RecipeStatus
{
    /// The entity that carries the recipe.
    ECS::EntityHandle Recipe{};
    /// Where its build stands.
    BuildState State = BuildState::Pending;
};

} // namespace GameEngine::SplineRecipes
