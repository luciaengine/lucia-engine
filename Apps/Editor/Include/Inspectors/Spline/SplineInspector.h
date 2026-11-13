#pragma once

#include <string>

namespace GameEngine
{

namespace SplineRecipes { class RecipeControllers; }
class UIElement;
struct InspectorContext;

void RegisterSplineInspectors();

// Registers the inspector of the wall: the build state notice above its reflected rows.
void RegisterSplineWallInspector();
// Registers the inspector of the extrude: the build state notice, the width mode, then only the
// rows the width mode and the profile use.
void RegisterSplineExtrudeInspector();

// Adds the notice with the build state of the recipe on `context.Entity` (Extrude, Wall or
// Placement): the state, and the reason when the recipe waits, fails or builds nothing. The notice
// follows the state while the inspector shows.
void AddSplineRecipeBuildStateNotice(const InspectorContext& context);

// The running recipe system's controllers; null in a process with no recipe system.
const SplineRecipes::RecipeControllers* RunningRecipeControllers();

// Makes the asset field under `rows` show `emptyText` while it holds nothing, for a field whose empty
// value draws another material. Returns the field's row; null when `rows` holds no asset field.
UIElement* ShowAssetFieldEmptyAs(UIElement& rows, std::string emptyText);

} // namespace GameEngine
