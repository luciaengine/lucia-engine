
#include "ForestPlacementFeature.h"
#include "ForestPlacementSession.h"
#include "ForestStandDrawProducer.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <array>
#include <span>
#include <utility>

namespace GameEngine::Engine::Renderer
{
namespace
{

// What one registered stand casts, shared by its depth-emit callback.
struct ShadowCasterSource
{
    uint64 WorldId;
    Rendering::RenderLayerMask Layers;
    std::shared_ptr<ForestPlacementSession> Session;
    EmitForestShadowOverview Overview;
};

void EmitShadowCasters(const ShadowCasterSource& source, DepthEmitContext& context, DepthPassType passType)
{
    auto* session = source.Session.get();
    if (passType != DepthPassType::ShadowCascade || !context.Frame || !context.Cascades || !context.Services ||
        context.Cascades->Count == 0 || context.Cascades->Count > kMaxDepthEmitCascades || (session && !session->CastsShadows))
        return;
    auto& services = *context.Services;
    const auto* camera = FindForestStandCamera(services, context.ViewId, source.WorldId, source.Layers);
    auto* feature = services.GetFeature<ForestPlacementFeature>();
    if (!camera || camera->renderOriginSector[3] == 0)
        return;
    const DepthEmitCascades& cascades = *context.Cascades;
    // The cascades and the cards share the camera's render origin (CascadeFrameData::RenderOriginSector).
    if (cascades.RenderOriginSector != CameraRenderOriginSector(*camera))
    {
        static bool warned = false;
        if (!std::exchange(warned, true))
            LOG_WARNING("Forest: the sun cascades of view {} are not in the camera's render origin. The forest "
                        "casts no shadow in that view.", context.ViewId);
        return;
    }
    if (session && !context.Frame->RequireExecutionLifetime(source.Session))
        return;

    const ForestPlacementView view = MakeForestStandView(*camera);
    const auto origin = Mathematics::WorldPosition::FromSector(cascades.RenderOriginSector, {});
    std::array<ForestPlacementView, kMaxDepthEmitCascades> cascadeViews{};
    for (uint32 c = 0; c < cascades.Count; ++c)
        cascadeViews[c] = {origin, cascades.Cascades[c].CasterPlanes, session ? session->DetailDistance : 0.0f};
    const Material* material = nullptr;
    if (session && feature && !session->Refused)
    {
        const auto pages = session->Placements->GatherShadowPages(std::span(cascadeViews.data(), cascades.Count),
                                                                  view.Eye, session->DetailDistance);
        if (feature->PlanShadowFrame(*session, context, view, pages) && session->Card && !session->Plan.ShadowDraws.empty())
            material = services.Materials().Registry().Find(session->Card->Material);
    }
    for (uint32 c = 0; c < cascades.Count; ++c)
    {
        std::span<const ForestDetailDraw> drawn;
        if (material && feature->EmitCascadeCasters(*session, context, c, *material, view))
        {
            drawn = session->CascadeDraws;
            if (!session->EmittedShadows)
            {
                session->EmittedShadows = true;
                LOG_INFO("Forest: registered packed impostor shadow casters with {} pages", drawn.size());
            }
        }
        if (source.Overview)
            source.Overview(context, c, cascadeViews[c], drawn);
    }
}

} // namespace

ScopedSubscription RegisterForestShadowCasters(RenderServices& services, uint64 worldId,
    Rendering::RenderLayerMask layers, std::shared_ptr<ForestPlacementSession> session, EmitForestShadowOverview overview)
{
    if ((!session && !overview) || layers == Rendering::RenderLayerMask::None())
        return {};
    auto source = std::make_shared<const ShadowCasterSource>(ShadowCasterSource{worldId, layers, std::move(session), std::move(overview)});
    return services.RegisterDepthEmit([source](DepthEmitContext& context, DepthPassType passType)
                                      { EmitShadowCasters(*source, context, passType); });
}

} // namespace GameEngine::Engine::Renderer
