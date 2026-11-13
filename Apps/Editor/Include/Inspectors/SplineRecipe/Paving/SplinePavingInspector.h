#pragma once

#include <string>

namespace GameEngine
{

struct InspectorContext;

namespace ECS
{
class World;
}

namespace SplineRecipes
{
struct PavingJunctionStatus;
struct PavingStatus;
}

/// Adds the paving's rows under `context.Parent`: the build notice, the header badge for roads not
/// paved, and the Surface, Curb, Road Mouths, Placement, Splines It Touches, Junctions and Shadows
/// sections. `status` is the result of the last rebuild, or null before the first.
void AddSplinePavingRows(const InspectorContext& context, const SplineRecipes::PavingStatus* status);

/// A junction's state in the words of the Junctions rows: "paved", "paved by <name>", "<d> m below
/// the paving". The Scene View's junction markers say the same words.
std::string PavingJunctionStateWords(const ECS::World& world, const SplineRecipes::PavingJunctionStatus& junction);

/// Registers the paving's inspector with the inspector registry.
void RegisterSplinePavingInspector();

} // namespace GameEngine
