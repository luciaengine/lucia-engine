#include "Engine/Rendering/SceneProbeWorldEpochs.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine
{
namespace Engine::Renderer
{

SceneProbeWorldEpochs SceneProbeWorldEpochs::Read(const RenderServices& services, uint64_t worldId)
{
    SceneProbeWorldEpochs epochs{};
    epochs.RenderContent = services.RenderContentVersion(worldId);
    epochs.ShadowCasters = services.ShadowCasterContentVersion(worldId);
    epochs.Lights = services.WorldLightListVersion(worldId);
    epochs.DynamicDepth = services.GetIdleElisionFrameState().DepthDynamicEpoch;
    epochs.Materials = Material::GetGlobalContentEpoch();
    epochs.Pipelines = services.Materials().PipelineReadinessEpoch();
    return epochs;
}

} // namespace Engine::Renderer
} // namespace GameEngine
