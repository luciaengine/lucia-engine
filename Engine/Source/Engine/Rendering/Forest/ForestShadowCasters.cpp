#include "ForestShadowCasters.h"

#include "ForestImpostorDraw.h"
#include "ForestPlacementFeature.h"
#include "ForestPlacementGpuData.h"
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
};

// Culls the cascade's resident placements to its caster planes and emits their light-facing cards.
void EmitCascadeCasters(ForestPlacementFeature& feature, ForestPlacementSession& session,
    const DepthEmitContext& context, uint32 cascade, const Material& material, ForestPlacementView view)
{
    const auto bit = static_cast<uint8>(1u << cascade);
    session.CascadeDraws.clear();
    for (const auto& draw : session.Plan.ShadowDraws)
        if ((draw.CascadeMask & bit) != 0)
            session.CascadeDraws.push_back(draw.Draw);
    if (session.CascadeDraws.empty())
        return;

    const auto& cascades = *context.Cascades;
    const auto origin = Mathematics::WorldPosition::FromSector(cascades.RenderOriginSector, {});
    const auto eyeOffset = view.Eye - origin;
    view.EyeRelativePlanes = cascades.Cascades[cascade].CasterPlanes;
    for (auto& plane : view.EyeRelativePlanes)
        plane.w += plane.x * eyeOffset.x + plane.y * eyeOffset.y + plane.z * eyeOffset.z;
    const auto owners = feature.EmitNearShadowMeshes(session, context, cascade, view);

    const auto& card = *session.Card;
    const uint32 slotCount = session.Placements->SlotCount();
    ForestPlacementGpuView gpuView{};
    if (!BuildForestPlacementGpuInputs(*session.Placements, session.CascadeDraws,
            MakeForestShadowGpuViewSource(*context.Cascades, cascade), slotCount, ForestCardReach(card),
            card.Bounds, gpuView, session.Pages, owners))
        return;
    const auto buffers = session.ShadowCulls[cascade].Declare(*context.Frame, session.ShadowScopes[cascade],
                                                              session.Slots, slotCount, gpuView, session.Pages);
    if (buffers)
        EmitForestImpostorShadowDraw(context, cascade, *buffers, session.Indices, card.Sway,
                                     DrawCommand{.Material = &material});
}

void EmitShadowCasters(const ShadowCasterSource& source, DepthEmitContext& context, DepthPassType passType)
{
    auto& session = *source.Session;
    if (passType != DepthPassType::ShadowCascade || !context.Frame || !context.Cascades || !context.Services ||
        context.Cascades->Count == 0 || !session.CastsShadows || session.Refused)
        return;
    auto& services = *context.Services;
    const auto* camera = FindForestStandCamera(services, context.ViewId, source.WorldId, source.Layers);
    auto* feature = services.GetFeature<ForestPlacementFeature>();
    if (!camera || camera->renderOriginSector[3] == 0 || !feature)
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
    if (!context.Frame->RequireExecutionLifetime(source.Session))
        return;

    const ForestPlacementView view = MakeForestStandView(*camera);
    const auto origin = Mathematics::WorldPosition::FromSector(cascades.RenderOriginSector, {});
    std::array<ForestPlacementView, kMaxDepthEmitCascades> cascadeViews{};
    for (uint32 c = 0; c < cascades.Count; ++c)
        cascadeViews[c] = {origin, cascades.Cascades[c].CasterPlanes, session.DetailDistance};
    const auto pages = session.Placements->GatherShadowPages(std::span(cascadeViews.data(), cascades.Count),
                                                             view.Eye, session.DetailDistance);
    if (!feature->PlanShadowFrame(session, context, view, pages) || session.Plan.ShadowDraws.empty() ||
        !session.Card)
        return;
    const Material* material = services.Materials().Registry().Find(session.Card->Material);
    if (!material)
        return;
    for (uint32 c = 0; c < cascades.Count; ++c)
        EmitCascadeCasters(*feature, session, context, c, *material, view);
    if (!session.EmittedShadows)
    {
        session.EmittedShadows = true;
        LOG_INFO("Forest: registered packed impostor shadow casters with {} pages", session.Plan.ShadowDraws.size());
    }
}

} // namespace

ScopedSubscription RegisterForestShadowCasters(RenderServices& services, uint64 worldId,
    Rendering::RenderLayerMask layers, std::shared_ptr<ForestPlacementSession> session)
{
    if (!session || layers == Rendering::RenderLayerMask::None())
        return {};
    auto source = std::make_shared<const ShadowCasterSource>(ShadowCasterSource{worldId, layers, std::move(session)});
    return services.RegisterDepthEmit([source](DepthEmitContext& context, DepthPassType passType)
                                      { EmitShadowCasters(*source, context, passType); });
}

} // namespace GameEngine::Engine::Renderer
