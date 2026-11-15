#pragma once

#include "ECS/ChangeFilter.h"
#include "ECS/Entity.h"
#include "Types/Types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine
{
class ContextMenuBuilder;
class UIElement;
namespace ECS { class World; }
} // namespace GameEngine

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// The Hierarchy's layer column: per row, a render layer picker for an entity with a MeshRenderer and a
/// sorting layer picker for an entity whose renderer sorts (a 2D sprite, a tile map), each shown when its
/// Hierarchy Options entry is on. A pick goes through the undo service and applies to `targets`. Main thread.
class HierarchyLayerControls
{
  public:
    using TargetsResolver = std::function<std::vector<ECS::EntityHandle>()>;

    /// Shows or hides the pickers on `row` for `entity`, creating each on its first need. `panelWidth` picks
    /// the narrow layout below 400 px.
    void Present(ECS::World& world, ECS::EntityHandle entity, UIElement& row, float panelWidth,
                 TargetsResolver targets, UndoRedoService* undo, EditorChangeNotifications* notifications);
    /// Hides the pickers of a pooled row that shows no entity.
    void Hide(UIElement& row);
    /// Adds the Show Render Layer and Show Sorting Layer entries to the Hierarchy Options menu.
    void BuildMenu(ContextMenuBuilder& builder) const;
    /// True when `command` was one of the BuildMenu entries; the rows then need a rebind.
    bool HandleCommand(uint32_t command);
    /// True once per change that the shown sorting pickers must follow: the project's sorting layers, a
    /// RenderSorting edit, another world or a world reset.
    bool NeedsRefresh(ECS::World& world);

  private:
    ECS::ChangeGate m_SortingChanges;
    const ECS::World* m_World = nullptr;
    uint64 m_ResetGeneration = 0;
    uint64 m_SortingLayersRevision = 0;
    bool m_ShowRenderLayer = false;
    bool m_ShowSortingLayer = false;
};

} // namespace GameEngine::Editor
