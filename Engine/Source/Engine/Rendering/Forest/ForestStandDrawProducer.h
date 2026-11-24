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
struct ForestPlacementView;

struct ForestStandDrawHeads
{
    DrawCommand Color;
    std::optional<DrawCommand> Depth;
};

/// Resolves the stand geometry, shaded color head and optional matching depth head.
/// Draw bindings must remain valid for the declaration. An absent result means
/// the stand mesh or its material is still loading.
using PrepareForestStandDraw = std::function<std::optional<ForestStandDrawHeads>(const ForwardEmitContext&)>;

/// The view that a stand plans for `camera`: the camera eye, and the frustum planes relative to it.
ForestPlacementView MakeForestStandView(const Rendering::CameraData& camera);
/// The camera of `viewId` when it is a view of world `worldId` that shows one of `layers`, else null.
const Rendering::CameraData* FindForestStandCamera(RenderServices& services, Rendering::ViewId viewId,
                                                   uint64 worldId, Rendering::RenderLayerMask layers);

/// Registers the stand in the world forward draw stream. The caller owns the
/// subscription for the scene's lifetime. Uses each view's own camera and layers. Main thread.
[[nodiscard]] ScopedSubscription RegisterForestStandDraws(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, PrepareForestStandDraw prepareDraw,
    std::shared_ptr<ForestPlacementSession> details = {});

/// Registers the resident trees' sun-shadow casters. The stand owns the subscription.
[[nodiscard]] ScopedSubscription RegisterForestShadowCasters(RenderServices& services, uint64 worldId,
    Rendering::RenderLayerMask layers, std::shared_ptr<ForestPlacementSession> session);

} // namespace GameEngine::Engine::Renderer
