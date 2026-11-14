#pragma once
#include "ECS/Systems.h"
#include "Engine/Rendering/PlanarReflectionMath.h"
#include <vector>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
class PlanarReflectionSystem : public ECS::ISystem
{
public:
    explicit PlanarReflectionSystem(RenderServices* services) : m_Services(services) {}
    const char* GetName() const override { return "PlanarReflectionSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
    static std::vector<PlanarReflectionPlane> ResolvePlanes(ECS::World& world);
private:
    RenderServices* m_Services = nullptr;
};
}
