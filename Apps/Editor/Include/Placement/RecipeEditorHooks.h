#pragma once

#include "Events/Event.h"
#include "SplineRecipes/RecipeEditJournal.h"

namespace GameEngine::SplineRecipes { class SplineRecipeSystem; }

namespace GameEngine::Editor
{

class UndoRedoService;

class RecipeEditorHooks final : public SplineRecipes::RecipeEditJournal
{
public:
    RecipeEditorHooks(UndoRedoService* undo, SplineRecipes::SplineRecipeSystem& recipes);
    ~RecipeEditorHooks() override;

    void RecordGroundEdit(ECS::World& world, const SplineRecipes::SplineGroundEdit& edit) override;
    void ReleaseSceneScoped(ECS::World& world);

private:
    UndoRedoService* m_Undo;
    SplineRecipes::SplineRecipeSystem& m_Recipes;
    EventSubscription m_PointEditSubscription;
};

} // namespace GameEngine::Editor
