#pragma once

#include "ECS/Systems.h"
#include "Engine/Rendering/MaterialMipStreaming/MaterialScreenDemand.h"
#include "Types/Types.h"

namespace GameEngine::Engine::Renderer
{

class RenderServices;

/// Gives material mip streaming its demand: the screen coverage of each material from the main
/// camera. It runs after extraction, in the world that the main camera draws, and does nothing
/// while streaming is off. An orthographic main camera gives no demand.
class MaterialMipDemandSystem : public ECS::ISystem
{
public:
    explicit MaterialMipDemandSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "MaterialMipDemand"; }
    // It reads the GPUScene instance rows, which extraction and the residency systems write.
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    RenderServices* m_RenderServices = nullptr;
    MaterialScreenDemand m_Demand;
    bool m_HasPublished = false;
};

} // namespace GameEngine::Engine::Renderer
