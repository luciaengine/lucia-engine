#pragma once

#include "Rendering/RenderLayerMask.h"
#include "Types/ScopedSubscription.h"
#include "Types/Types.h"

#include <memory>

namespace GameEngine::Engine::Renderer
{

class RenderServices;
struct ForestPlacementSession;

/// Registers the stand's sun-shadow casters as a depth producer: the light-facing cards of the
/// resident trees within the session's Detail Distance, in each sun cascade of the view that draws
/// the trees. The session's CastsShadows turns them on and off. The caller owns the subscription for
/// the scene's lifetime. Main thread.
[[nodiscard]] ScopedSubscription RegisterForestShadowCasters(RenderServices& services, uint64 worldId,
    Rendering::RenderLayerMask layers, std::shared_ptr<ForestPlacementSession> session);

} // namespace GameEngine::Engine::Renderer
