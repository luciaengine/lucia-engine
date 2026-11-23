#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/ScopedSubscription.h"

#include <functional>
#include <memory>
#include <optional>

namespace GameEngine::Engine::Renderer
{

class ForestPlacementRuntime;
class RenderServices;
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
/// subscription for the scene's lifetime. Uses each view's own camera and layers. Main thread.
[[nodiscard]] ScopedSubscription RegisterForestStandDraws(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, PrepareForestStandDraw prepareDraw,
    std::shared_ptr<ForestPlacementSession> details = {});

} // namespace GameEngine::Engine::Renderer
