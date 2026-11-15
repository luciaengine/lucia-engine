#pragma once

#include "ECS/Systems.h"

namespace GameEngine::Engine::Renderer
{
class ClusterLODFeature;

class ClusterLODPrepareSystem : public ECS::ISystem
{
  public:
    explicit ClusterLODPrepareSystem(ClusterLODFeature* feature) : m_Feature(feature) {}
    const char* GetName() const override { return "ClusterLODPrepare"; }
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;

  private:
    ClusterLODFeature* m_Feature;
};
} // namespace GameEngine::Engine::Renderer
