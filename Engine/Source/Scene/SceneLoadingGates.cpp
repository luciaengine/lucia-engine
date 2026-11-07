#include "SceneLoadingGates.h"

#include "Core/Engine.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine
{
SceneLoadingGates::SceneLoadingGates(EngineCore& engine)
    : m_Resolve(engine.GetSceneLoadTracker(), {"scene.resolve", "Preparing the scene"},
              "models or materials still resolving", [&engine]() {
                  size_t pending = Engine::Renderer::IsSceneBuildPumpActive() ? 1 : 0;
                  if (auto* loop = engine.GetRenderingLoop())
                  {
                      const auto counts = loop->GetSceneResolveService().Counts();
                      pending += counts.HeldEntities + counts.PendingMaterials;
                  }
                  return pending;
              }),
      m_BaseCompiles(engine.GetSceneLoadTracker(),
                   {"shader.base-compile", "Compiling material shaders", SceneLoadTracker::CostUnit::Pipelines},
                   "base shaders still compiling or waiting to publish", [&engine]() -> size_t {
                       auto* render = engine.GetRenderServices();
                       return render ? render->Materials().OutstandingBaseCompileCount() : 0;
                   }),
      m_Variants(engine.GetSceneLoadTracker(),
               {"shader.variant", "Compiling draw variants", SceneLoadTracker::CostUnit::Variants},
               "draw variants still compiling or waiting to publish", [&engine]() -> size_t {
                   auto* render = engine.GetRenderServices();
                   return render ? render->Materials().Variants().OutstandingVariantCount() : 0;
               }),
      m_DevicePipelines(engine.GetSceneLoadTracker(),
                      {"gpu.pipeline", "Building device pipelines", SceneLoadTracker::CostUnit::Pipelines},
                      "device pipelines still queued or building", [&engine]() -> size_t {
                          auto* render = engine.GetRenderServices();
                          auto* device = render ? render->GetDevice() : nullptr;
                          return device ? device->PendingPipelineBuildCount() : 0;
                      }),
      m_Textures(engine.GetSceneLoadTracker(), {"render.texture-upload", "Loading textures"},
               "material textures still waiting to bind", [&engine]() -> size_t {
                   auto* render = engine.GetRenderServices();
                   return render ? render->Textures().PendingMaterialTextureBindCount() : 0;
               })
{
}

} // namespace GameEngine
