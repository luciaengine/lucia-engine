#pragma once

#include "ECS/Entity.h"
#include <functional>
#include <vector>

namespace GameEngine { class UIElement; class ContextMenuBuilder; }
namespace GameEngine::Editor
{
class UndoRedoService;
class EditorChangeNotifications;

/// Owns layer pickers and their display options for one hierarchy.
class HierarchyLayerControls
{
public:
    void Present(ECS::World& world, ECS::EntityHandle entity, UIElement& row, UIElement& title, float panelWidth,
        const std::function<std::vector<ECS::EntityHandle>()>& targets,
        UndoRedoService* undo, EditorChangeNotifications* notifications);
    void BuildMenu(ContextMenuBuilder& builder) const;
    bool HandleCommand(uint32_t command);
    /// Whether visibility-mask edits need to refresh the displayed column.
    bool IsRenderLayerVisible() const { return m_ShowVisibility; }
    /// Consumes changes to project layer names and priorities.
    bool NeedsRefresh(ECS::World& world);
    /// Clears entity controls when a pooled row represents a non-entity.
    void Hide(UIElement& row);
private:
    ECS::ChangeGate m_Changes;
    const ECS::World* m_World = nullptr;
    uint64_t m_ResetGeneration = 0;
    uint64_t m_LayersRevision = 0;
    bool m_ShowVisibility = false;
    bool m_ShowSorting = false;
};
}
