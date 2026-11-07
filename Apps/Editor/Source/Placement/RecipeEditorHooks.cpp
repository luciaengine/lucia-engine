#include "Placement/RecipeEditorHooks.h"

#include "Placement/FenceSpanOverrideRemap.h"
#include "SplineGroundEditCommand.h"
#include "UndoRedo/SplinePointEdits.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/Systems.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipeSystem.h"

#include <memory>

namespace GameEngine::Editor
{

RecipeEditorHooks::RecipeEditorHooks(UndoRedoService* undo, SplineRecipes::SplineRecipeSystem& recipes)
    : m_Undo(undo),
      m_Recipes(recipes),
      m_PointEditSubscription(SplinePointEdited().Subscribe(
          [&recipes](const SplinePointEdit& edit)
          { RemapFenceOverridesOnPointEdit(edit, recipes.GetControllers().FenceRunSpanCounts(edit.Entity)); }))
{
    m_Recipes.SetEditJournal(this);
}

RecipeEditorHooks::~RecipeEditorHooks()
{
    m_Recipes.SetEditJournal(nullptr);
}

void RecipeEditorHooks::RecordGroundEdit(ECS::World& world, const SplineRecipes::SplineGroundEdit& edit)
{
    if (!m_Undo)
        return;
    const auto action = m_Undo->GetPendingActionId();
    if (action != 0)
        m_Undo->AppendConsequence(action, std::make_unique<SplineGroundEditCommand>(&world, edit));
    else if (edit.Removes())
        m_Undo->CommitAlreadyApplied(std::make_unique<SplineGroundEditCommand>(&world, edit));
}

void RecipeEditorHooks::ReleaseSceneScoped(ECS::World& world)
{
    m_Recipes.ReleaseSceneScoped(world);
}

} // namespace GameEngine::Editor
