#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Forest/ForestPlacementCull.h"

namespace GameEngine::Engine::Renderer
{

struct ForwardEmitContext;
struct ImpostorFoliageSway;

/// Emits the cards of the culled placements with the packed card material in color.Material, in the
/// color pass. cardIndices holds the indices 0 through 5; its owner keeps it alive.
bool EmitForestImpostorDraw(const ForwardEmitContext& context,
    const ForestPlacementCull::DrawBuffers& buffers, Rendering::BufferHandle cardIndices,
    const ImpostorFoliageSway& sway, DrawCommand color);

} // namespace GameEngine::Engine::Renderer
