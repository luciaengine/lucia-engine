#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"
#include <vector>

namespace GameEngine { struct InspectorContext; }
namespace GameEngine::ECS { class World; }
namespace GameEngine::Editor
{
class UndoRedoService;
class EditorChangeNotifications;
/// Registers the authored rendering order inspector and project settings.
void RegisterRenderSortingEditor();
/// Shows effective layer, order, and outermost group using the renderer resolver.
void AddRenderSortingDiagnostic(const InspectorContext& context);
/// Registers a renderer component whose entities expose the hierarchy sorting picker. Main-thread only.
void RegisterRenderSortingComponent(ECS::ComponentTypeId component);
/// Whether the entity can author an ordering layer in the Hierarchy.
bool SupportsRenderSorting(ECS::World& world, ECS::EntityHandle entity);
/// Assigns one layer to the selection in a single undo step, preserving component absence.
void ApplyRenderSortingLayer(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
    int32 layer, UndoRedoService* undo, EditorChangeNotifications* notifications);
}
