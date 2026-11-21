#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Forest/ForestPlacementCull.h"

namespace GameEngine::Engine::Renderer
{

struct ForwardEmitContext;
struct ImpostorFoliageSway;

/// Emit the prepared card material and its optional camera depth head. The
/// index buffer contains the sequence 0 through 5. Its owner keeps it alive.
bool EmitForestImpostorDraw(const ForwardEmitContext& context,
    const ForestPlacementCull::DrawBuffers& buffers, Rendering::BufferHandle cardIndices,
    const ImpostorFoliageSway& sway, DrawCommand color, const DrawCommand* depth);

} // namespace GameEngine::Engine::Renderer
