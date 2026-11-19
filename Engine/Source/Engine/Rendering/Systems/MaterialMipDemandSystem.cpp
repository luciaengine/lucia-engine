#include "ECSModules/Rendering/Systems/MaterialMipDemandSystem.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/GPUScene.h"

#include "ECS/World.h"

namespace GameEngine::Engine::Renderer
{

namespace
{
// A sweep of 1,000,000 rows takes about 120 updates. A small scene's sweep takes 30 updates, so a
// moving camera changes the stream order about 2 times per second at 60 frames per second.
constexpr MaterialScreenDemand::Pace kSweepPace{8192u, 30u};
} // namespace

bool MaterialMipDemandSystem::IsEnabled() const
{
    // The render services hold a GPUScene only while they also hold the texture service.
    return Enabled && m_RenderServices && m_RenderServices->GetGPUScene() &&
           m_RenderServices->Textures().StreamsMaterialMips();
}

void MaterialMipDemandSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    // One camera orders the streams: the main camera over every world. Only its world sweeps.
    const ViewRegistry& views = m_RenderServices->Views();
    const Rendering::CameraData* camera = views.FindMainCameraData(0);
    if (!camera || camera != views.FindMainCameraData(world.GetWorldId()))
        return;

    TextureService& textures = m_RenderServices->Textures();
    const std::optional<MaterialScreenDemand::View> view = MaterialScreenDemand::ViewOf(*camera);
    if (!view)
    {
        if (m_HasPublished)
        {
            m_Demand.Reset();
            textures.UpdateMaterialMipDemand({});
            m_HasPublished = false;
        }
        return;
    }

    if (m_Demand.Advance(m_RenderServices->GetGPUScene()->GetInstances(), *view, kSweepPace))
    {
        textures.UpdateMaterialMipDemand(m_Demand.Coverage());
        m_HasPublished = true;
    }
}

} // namespace GameEngine::Engine::Renderer
