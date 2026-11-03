#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"
#include <functional>
#include <utility>
#include <vector>

namespace GameEngine { struct InspectorContext; }
namespace GameEngine::ECS { class World; }
namespace GameEngine::Editor
{
class UndoRedoService;
class EditorChangeNotifications;
/// Registers the authored rendering order inspector and project settings.
void RegisterRenderSortingEditor();
/// Adds editable layer and order rows, including implicit Default / 0 values.
void AddRenderSortingFields(const InspectorContext& context);
/// Creates one undoable order edit per gesture, preserving absent sorting components.
std::pair<std::function<void(int)>, std::function<void(int)>>
MakeRenderSortingOrderHandlers(const InspectorContext& context);
/// Shows an explanation only when an ancestor group overrides layer and order.
void AddRenderSortingDiagnostic(const InspectorContext& context);
/// Registers a renderer component whose entities expose the hierarchy sorting picker. Main-thread only.
void RegisterRenderSortingComponent(ECS::ComponentTypeId component);
/// Whether a registered renderer exposes inline sorting fields on this entity.
bool HasSortingRenderer(ECS::World& world, ECS::EntityHandle entity);
/// Whether the entity can author an ordering layer in the Hierarchy.
bool SupportsRenderSorting(ECS::World& world, ECS::EntityHandle entity);
/// Assigns one layer to the selection in a single undo step, preserving component absence.
void ApplyRenderSortingLayer(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
    int32 layer, UndoRedoService* undo, EditorChangeNotifications* notifications);
}
