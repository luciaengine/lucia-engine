#pragma once

#include "ECS/Entity.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// Sets each entity's MeshRenderer::RenderLayers to `mask`, recorded as ONE undo step, so a multi-row edit
/// undoes in one press. An entity whose layers already are `mask`, and an entity with no MeshRenderer,
/// contributes nothing to the undo step.
void ApplyRenderLayerMask(
    ECS::World& world,
    const std::vector<ECS::EntityHandle>& entities,
    Rendering::RenderLayerMask mask,
    UndoRedoService* undo,
    EditorChangeNotifications* notifications);

}  // namespace GameEngine::Editor
