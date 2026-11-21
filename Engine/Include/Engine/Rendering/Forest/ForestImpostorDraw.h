#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Forest/ForestPlacementCull.h"

namespace GameEngine::Engine::Renderer
{

struct ForwardEmitContext;
struct ImpostorFoliageSway;

struct ForestSplatDrawData
{
    Rendering::BufferHandle Cells;
    uint64 CellBytes = 0;
    std::array<float, 4> Grid{};
    uint32 IndexCount = 0;
};

/// Emit the prepared card material and its optional camera depth head. The
/// index buffer contains the sequence 0 through 5. Its owner keeps it alive.
bool EmitForestImpostorDraw(const ForwardEmitContext& context,
    const ForestPlacementCull::DrawBuffers& buffers, Rendering::BufferHandle cardIndices,
    const ImpostorFoliageSway& sway, DrawCommand color, const DrawCommand* depth,
    const ForestSplatDrawData* splats = nullptr);

} // namespace GameEngine::Engine::Renderer
