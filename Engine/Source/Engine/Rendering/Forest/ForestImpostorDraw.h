#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "ForestPlacementCull.h"
#include "Mathematics/Vector4.h"

namespace GameEngine::Rendering { struct CameraData; }

namespace GameEngine::Engine::Renderer
{

struct DepthEmitContext;
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

/// Emits the cards of the culled placements into sun cascade `cascade` of the context's view, facing
/// the light, with the packed card material in card.Material. Call only from a depth producer that
/// received DepthEmitContext::Cascades.
bool EmitForestImpostorShadowDraw(const DepthEmitContext& context, uint32 cascade,
    const ForestPlacementCull::DrawBuffers& buffers, Rendering::BufferHandle cardIndices,
    const ImpostorFoliageSway& sway, DrawCommand card);

/// What a color card faces (forestCard.viewer in forest_impostor.glsl): the camera eye relative to
/// the render origin, with w = 1.
Mathematics::Vector4 ForestCardEyeViewer(const Rendering::CameraData& camera);

} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer
{
/// Emits the animated splats of the culled placements (forest_splat_cull.comp rows) with the forest splat
/// material in color.Material, in the color pass. The geometry's owner keeps its buffers alive.
bool EmitForestSplatDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    const ForestSplatGeometry& splats, DrawCommand color);
} // namespace GameEngine::Engine::Renderer
