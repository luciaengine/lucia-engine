#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowWorldPass.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include <optional>

namespace GameEngine::Engine::Renderer
{

bool TakesScreenSpaceShadowMask(RenderServices& services, uint64_t worldId, const ResolvedShadowSettings& settings,
                                bool transmissiveOnly, Rendering::MaterialKeyword keywords)
{
    return settings.ScreenSpaceShadows && !transmissiveOnly &&
           Rendering::HasKeyword(keywords, Rendering::MaterialKeyword::Shadows) &&
           TowardShadowCastingPrimaryDirectional(services.GetWorldLights(worldId)).has_value();
}

Rendering::MaterialKeyword ContributeScreenSpaceShadows(
    RenderServices& services, Rendering::RenderGraph::RGFrame& frame,
    const ScreenSpaceShadowView& view, std::unique_ptr<ScreenSpaceShadowPasses>& passes,
    Rendering::RenderGraph::RGTexture& mask, Rendering::MaterialKeyword keywords)
{
    using Rendering::MaterialKeyword;
    // A keyword is permission to sample a mask produced in this view/frame,
    // not a persisted pipeline option or merely an enabled volume setting.
    keywords = keywords & ~MaterialKeyword::ScreenSpaceShadows;
    if (!TakesScreenSpaceShadowMask(services, view.WorldId, view.Settings, view.TransmissiveOnly, keywords))
        return keywords;

    const bool canDeclareMask = view.CanDeclare && !mask.IsValid() && view.Depth.IsValid() && view.Camera;
    const auto towardLight = canDeclareMask
                                 ? TowardShadowCastingPrimaryDirectional(services.GetWorldLights(view.WorldId))
                                 : std::nullopt;
    if (towardLight)
    {
        if (!passes)
            passes = std::make_unique<ScreenSpaceShadowPasses>(services.GetDevice());
        ScreenSpaceShadowFilter filter{};
        const auto shadowMap = services.GetShadowMapArrayRG(frame, view.ViewId);
        // Match the current per-view PCF footprint, not a second fit.
        // Only the fixed grid kernels have one: a ray-traced mask and the
        // blocker-dependent filters (PCSS, DPCF, MSM) have no footprint to
        // project, and asking for one would schedule a pass that copies.
        const bool gridFilter =
            view.Settings.Filter == Components::DirectionalShadowFilter::Grid5x5 ||
            view.Settings.Filter == Components::DirectionalShadowFilter::Grid3x3;
        if (gridFilter && !view.HasRayTracedMask && shadowMap.IsValid())
        {
            const auto* instance = services.Spine().PipelineInstanceForFrame(frame);
            const auto* resources = instance ? instance->FrameResourcesFor(&frame) : nullptr;
            if (const auto binding = resources ? resources->FindBufferBinding(view.ViewId, "ShadowData")
                                               : Pipeline::PipelineBufferBindingRG{};
                binding.IsValid())
            {
                filter.Constants = binding.Buffer;
                filter.Offset = binding.Offset;
                filter.Bytes = binding.Size;
                filter.Resolution = frame.Graph().ResourceDesc(shadowMap.Id).Width;
                filter.Quality = static_cast<uint32_t>(view.Settings.Filter);
            }
        }
        mask = passes->DeclareMaskPass(frame, view.ViewId, view.Depth, *view.Camera,
            towardLight->v, view.Settings.ScreenSpaceShadowThickness, &filter);
    }
    return mask.IsValid() ? keywords | MaterialKeyword::ScreenSpaceShadows : keywords;
}
} // namespace GameEngine::Engine::Renderer
