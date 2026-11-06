#pragma once

// Recipe build state for the current world. Read it on the main thread.

#include "ECS/ECS.h"

#include <cstdint>
#include <span>
#include <string>

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

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

/// The build state of `recipe`. A recipe the recipe system has not updated yet is Pending. An
/// entity with no recipe is Built: it has nothing to build.
RecipeStatus StateOf(ECS::World& world, ECS::EntityHandle recipe);

/// True when no recipe in the world is Pending or Held: every recipe is Built or Failed. A world
/// with no recipe is settled. Poll it each frame: an edit or a late asset can unsettle it again.
bool IsSceneSettled(ECS::World& world);

/// Writes every Failed recipe into `failures` and returns how many there are, also when that is
/// more than `failures` holds.
std::int32_t Failures(ECS::World& world, std::span<RecipeStatus> failures);

/// Why `recipe` is Held or Failed, or why a Built recipe made nothing, in words that name the
/// fix. Empty for a recipe that is Pending, or Built with output.
std::string ReasonOf(ECS::World& world, ECS::EntityHandle recipe);

/// IsSceneSettled as the recipe system found it at the end of its last update of `world`. Before that
/// update, and for a world it does not update, it is IsSceneSettled. A system of the schedule can call
/// it on a job: the recipe system's update is exclusive, so the two never run at the same time.
bool IsSceneSettledAtLastUpdate(ECS::World& world);

} // namespace GameEngine::SplineRecipes
