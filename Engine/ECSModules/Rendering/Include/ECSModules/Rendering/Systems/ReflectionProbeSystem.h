#pragma once

#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Types/Types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

class ReflectionProbeSystem : public ECS::ISystem
{
  public:
    explicit ReflectionProbeSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "ReflectionProbeSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

  private:
    // True when nothing the probe descriptors read changed since the last rebuild.
    bool SceneUnchangedSinceLastBuild(ECS::World& world);

    RenderServices* m_RenderServices = nullptr;

    // Descriptors from the last full rebuild, re-sent unchanged on idle frames.
    std::vector<SceneReflectionProbeEnvironmentDesc> m_Probes;
    uint64_t m_BuiltWorldId = 0;
    std::size_t m_BuiltStructuralVersion = 0;
    uint64_t m_BuiltMaterialDigest = 0;
    bool m_HasBuild = false;
    ECS::ChangeGate m_ProbeGate;
    ECS::ChangeGate m_MeshGate;
    ECS::ChangeGate m_TransformGate;
    ECS::ChangeGate m_BoundsGate;
    ECS::ChangeGate m_LightGate;
};

} } // namespace GameEngine::Engine::Renderer
