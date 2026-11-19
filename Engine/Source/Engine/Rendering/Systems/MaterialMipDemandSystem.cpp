#include "ECSModules/Rendering/Systems/MaterialMipDemandSystem.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/GPUScene.h"

#include "ECS/World.h"

#include <cmath>

namespace GameEngine::Engine::Renderer
{

namespace
{
// Instance rows read per update. A sweep of 1,000,000 rows takes about 120 updates.
constexpr uint32 kRowsPerUpdate = 8192u;

// The camera's perspective Y scale, or 0 for a camera that gives no demand: an editor 2D view,
// or a camera with no eye yet.
float32 DemandProjScaleY(const Rendering::CameraData& camera)
{
    const bool orthographic2D = camera.cameraPos.w == 1.0f;
    const bool hasEye = camera.renderOriginSector[3] != 0;
    return orthographic2D || !hasEye ? 0.0f : std::abs(camera.proj[1][1]);
}
} // namespace

void MaterialMipDemandSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices)
        return;
    TextureService& textures = m_RenderServices->Textures();
    if (!textures.StreamsMaterialMips())
        return;

    // One camera orders the streams: the main camera over every world. Only its world sweeps.
    const ViewRegistry& views = m_RenderServices->Views();
    const Rendering::CameraData* camera = views.FindMainCameraData(0);
    if (!camera || camera != views.FindMainCameraData(world.GetWorldId()))
        return;

    const Rendering::GPUScene* scene = m_RenderServices->GetGPUScene();
    const float32 projScaleY = DemandProjScaleY(*camera);
    if (!scene || projScaleY <= 0.0f)
    {
        if (m_HasPublished)
        {
            m_Demand.Reset();
            textures.UpdateMaterialMipDemand({});
            m_HasPublished = false;
        }
        return;
    }

    MaterialScreenDemand::View view;
    view.OriginSector = CameraRenderOriginSector(*camera);
    view.Eye = CameraEyeRelativeToOrigin(*camera);
    view.ProjScaleY = projScaleY;
    if (m_Demand.Advance(scene->GetInstances(), view, kRowsPerUpdate))
    {
        textures.UpdateMaterialMipDemand(m_Demand.Coverage());
        m_HasPublished = true;
    }
}

} // namespace GameEngine::Engine::Renderer
