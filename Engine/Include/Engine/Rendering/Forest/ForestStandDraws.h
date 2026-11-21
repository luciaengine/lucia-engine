#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Types/ScopedSubscription.h"
#include "Rendering/RenderLayerMask.h"

#include <functional>
#include <memory>
#include <optional>

namespace GameEngine::Engine::Renderer
{

class ForestPlacementRuntime;
struct ForestPlacementSession;

struct ForestStandDrawHeads
{
    DrawCommand Color;
    std::optional<DrawCommand> Depth;
};

/// Resolves the stand geometry, shaded color head and optional matching depth head.
/// Draw bindings must remain valid for the declaration. An absent result means
/// the stand mesh or its material is still loading.
using PrepareForestStandDraw = std::function<std::optional<ForestStandDrawHeads>(const ForwardEmitContext&)>;

/// Registers the stand in the world forward draw stream. The caller owns the
/// subscription for the scene's lifetime. Uses each view's own camera and layers.
[[nodiscard]] ScopedSubscription RegisterForestStandDraws(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, PrepareForestStandDraw prepareDraw,
    std::shared_ptr<ForestPlacementSession> details = {});

/// Binds a cooked stand's asset-relative index ranges to a registered static mesh
/// and its resolved material. A null material means the entity or its material
/// is unavailable. The stand's vertices are in the archive's world space.
[[nodiscard]] ScopedSubscription RegisterForestStandMesh(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, const Rendering::MeshGPUKey& mesh,
    std::function<GUID()> resolveMaterial, std::shared_ptr<ForestPlacementSession> details = {});

} // namespace GameEngine::Engine::Renderer
