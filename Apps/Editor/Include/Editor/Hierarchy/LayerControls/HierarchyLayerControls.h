#pragma once

#include "ECS/ChangeFilter.h"
#include "ECS/Entity.h"
#include "Events/Event.h"
#include "Types/Types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine
{
class ContextMenuBuilder;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// The Hierarchy's optional layer columns: a Render Layer dropdown (the layer a MeshRenderer draws on) and a
/// Sorting Layer dropdown (the draw order of alpha-blended artwork), each turned on from Hierarchy Options.
/// A pick applies to the row's targets as one undo step. Main thread.
class HierarchyLayerControls
{
public:
    HierarchyLayerControls();

    /// Shows the turned-on columns of `entity` on `row`, and reserves their width on `title`. `targets` gives
    /// the entities a pick applies to: the row's entity, or the selection when the row is in it.
    void Present(ECS::World& world, ECS::EntityHandle entity, UIElement& row, UIElement& title, float panelWidth,
                 const std::function<std::vector<ECS::EntityHandle>()>& targets, UndoRedoService* undo,
                 EditorChangeNotifications* notifications);
    /// Hides the columns of a pooled row that shows no entity.
    void Hide(UIElement& row);

    /// Adds Show Render Layer and Show Sorting Layer to the Hierarchy Options menu.
    void BuildMenu(ContextMenuBuilder& builder) const;
    /// Toggles a column for its menu command. False for any other command.
    bool HandleCommand(uint32_t command);

    /// Whether the Render Layer column is on, so a render layer edit has to refresh the rows.
    bool IsRenderLayerVisible() const { return m_ShowVisibility; }
    /// True once after the sorting layers, the render layer names, the world or a shown RenderSorting changed.
    bool NeedsRefresh(ECS::World& world);

private:
    ECS::ChangeGate m_Changes;
    EventSubscription m_NamesRefreshed;
    const ECS::World* m_World = nullptr;
    uint64_t m_ResetGeneration = 0;
    uint64_t m_LayersRevision = 0;
    uint64_t m_NamesRevision = 1;
    uint64_t m_PresentedNamesRevision = 1;
    bool m_ShowVisibility = false;
    bool m_ShowSorting = false;
};

} // namespace GameEngine::Editor
