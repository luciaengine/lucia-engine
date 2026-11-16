#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

class RenderServices;

/// The epochs a scene reflection probe capture can see move between bakes
/// that its own input digest leaves out: moved or edited renderables, shadow
/// casters, lights, dynamic depth (terrain, ocean, grass, playing characters),
/// material properties, and draws that were skipped while their pipelines
/// built. Shader-time animation, playing video textures and blend-only forward
/// producers such as particles move none of them.
struct SceneProbeWorldEpochs
{
    uint64_t RenderContent = 0;
    uint64_t ShadowCasters = 0;
    uint64_t Lights = 0;
    /// Global, not per world: it advances every frame while any depth-writing
    /// producer or dynamic-depth feature is active anywhere.
    uint64_t DynamicDepth = 0;
    /// Global: Material::GetGlobalContentEpoch, moved by any material property
    /// edit or reload. A playing video texture uploads into the texture it is
    /// bound to and does not move it.
    uint64_t Materials = 0;
    /// Global: MaterialSystem::PipelineReadinessEpoch, moved when a draw the
    /// publish gate skipped (its pipeline variant compiling, or its pipeline
    /// cold for the capture's pass format) can draw. A capture taken while a
    /// pipeline built is missing that draw, and nothing else here moves when it
    /// lands; how long the build took (shader bytes, cache state, machine) then
    /// decided what the probe keeps.
    uint64_t Pipelines = 0;

    bool operator==(const SceneProbeWorldEpochs&) const = default;

    static SceneProbeWorldEpochs Read(const RenderServices& services, uint64_t worldId);
};

} // namespace Engine::Renderer
} // namespace GameEngine
