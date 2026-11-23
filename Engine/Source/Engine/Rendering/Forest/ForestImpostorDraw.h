#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "ForestPlacementCull.h"

namespace GameEngine::Engine::Renderer
{

struct ForwardEmitContext;
struct ForestSplatCells;
struct ImpostorFoliageSway;

/// The splat bake of a session on the GPU: the cells of ForestSplatCells and their disk indices.
struct ForestSplatGeometry
{
    const ForestSplatCells* Cells;
    Rendering::BufferHandle CellBuffer;
    Rendering::BufferHandle Indices;
};

/// Emits the cards of the culled placements with the packed card material in color.Material, in the
/// color pass. cardIndices holds the indices 0 through 5; its owner keeps it alive.
bool EmitForestImpostorDraw(const ForwardEmitContext& context,
    const ForestPlacementCull::DrawBuffers& buffers, Rendering::BufferHandle cardIndices,
    const ImpostorFoliageSway& sway, DrawCommand color);

} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer
{
/// Emits the animated splats of the culled placements (forest_splat_cull.comp rows) with the forest splat
/// material in color.Material, in the color pass. The geometry's owner keeps its buffers alive.
bool EmitForestSplatDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    const ForestSplatGeometry& splats, DrawCommand color);
} // namespace GameEngine::Engine::Renderer
