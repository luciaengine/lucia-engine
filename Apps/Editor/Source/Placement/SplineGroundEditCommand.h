#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "SplineRecipes/SplineGroundEdit.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

// The undo step of SplineRecipes::SplineGroundEdit: the terrain pieces that the ground authority
// added, rewrote or removed on one entity, with their SplineGroundProvisioned marker. A piece that
// the sweep did not change (a hand-authored one) is never in the step.
class SplineGroundEditCommand final : public IEditorCommand
{
public:
    SplineGroundEditCommand(ECS::World* world, const SplineRecipes::SplineGroundEdit& edit)
        : m_World(world)
        , m_Edit(edit)
    {
    }

    const char* GetName() const override { return "Update Recipe Ground Effects"; }
    const char* GetTypeName() const override { return "SplineGroundEditCommand"; }

    void Do() override
    {
        if (m_World)
            m_Edit.Apply(*m_World);
    }

    void Undo() override
    {
        if (m_World)
            m_Edit.Revert(*m_World);
    }

private:
    ECS::World* m_World = nullptr;
    SplineRecipes::SplineGroundEdit m_Edit;
};

} // namespace GameEngine::Editor
