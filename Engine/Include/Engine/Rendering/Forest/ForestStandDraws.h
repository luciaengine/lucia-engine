#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/ScopedSubscription.h"

#include <functional>
#include <memory>

namespace GameEngine::Rendering { struct MeshGPUKey; }

namespace GameEngine::Engine::Renderer
{

class ForestPlacementRuntime;
class RenderServices;
struct ForestPlacementSession;

/// Binds a cooked stand's asset-relative index ranges to a registered static mesh
/// and its resolved material. A null material means the entity or its material
/// is unavailable. The stand's vertices are in the archive's world space.
/// The caller owns the subscription for the scene's lifetime. Main thread.
[[nodiscard]] ScopedSubscription RegisterForestStandMesh(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, const Rendering::MeshGPUKey& mesh,
    std::function<GUID()> resolveMaterial, std::shared_ptr<ForestPlacementSession> details = {});

} // namespace GameEngine::Engine::Renderer
