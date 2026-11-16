#include "Engine/Rendering/VirtualShadows/VirtualShadowWorldPass.h"

#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ResolvedShadowSettings.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowRenderFeature.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowSunMask.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowViewInputs.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstring>

namespace GameEngine::Engine::Renderer
{
namespace RenderGraph = Rendering::RenderGraph;
using Rendering::MaterialKeyword;

namespace
{
// The view's ShadowData block as the pipeline published it this frame.
Pipeline::PipelineBufferBindingRG FindShadowData(RenderServices& services, const RenderGraph::RGFrame& frame,
                                                 uint32_t viewId)
{
    const auto* instance = services.Spine().PipelineInstanceForFrame(frame);
    const auto* resources = instance ? instance->FrameResourcesFor(&frame) : nullptr;
    return resources ? resources->FindBufferBinding(viewId, "ShadowData") : Pipeline::PipelineBufferBindingRG{};
}

void DeclareSunMask(RenderServices& services, RenderGraph::RGFrame& frame, const VirtualShadowSunMaskView& view,
                    VirtualShadowViewInputs& inputs)
{
    inputs.SunMask = {};
    auto* feature = services.GetFeature<VirtualShadowRenderFeature>();
    const auto towardLight = TowardShadowCastingPrimaryDirectional(services.GetWorldLights(view.WorldId));
    if (!feature || !towardLight)
        return;
    VirtualShadowSunMaskRequest request{};
    request.ViewId = view.ViewId;
    request.Depth = view.Depth;
    request.Camera = view.Camera;
    request.TowardLight = *towardLight;
    request.Inputs = &inputs;
    request.ShadowData = FindShadowData(services, frame, view.ViewId);
    request.Letterbox = view.Letterbox;
    inputs.SunMask = feature->DeclareSunMask(frame, request);
    inputs.SunMaskDepth = view.Depth;
    inputs.SunMaskViewProjection = view.Camera->viewProjRel;
}

// The sun takes its virtual page visibility from the mask only where the in-shader lookup runs:
// a cascade (not ray-traced) sun whose effective filter is Poisson, the one
// GE_TryVirtualShadowDirectional resolves. A PCSS request demotes to Poisson without bindless
// raw depth or on a device that prefers stable filtering.
bool SunReadsVirtualPages(RenderServices& services, uint32_t viewId, const ResolvedShadowSettings& settings)
{
    const auto* shadows = services.GetFeature<ShadowMapRenderFeature>();
    return settings.Mode == Components::DirectionalShadowMode::Cascades && shadows &&
           shadows->ResolveEffectiveFilterQuality(services, viewId) == ShadowFilterQuality::PoissonPCF;
}
} // namespace

MaterialKeyword ContributeVirtualShadows(const VirtualShadowViewInputs& inputs, MaterialKeyword keywords)
{
    keywords = keywords & ~(MaterialKeyword::VirtualShadowMaps | MaterialKeyword::VirtualShadowSunMask);
    if (!inputs.IsValid() || !Rendering::HasKeyword(keywords, MaterialKeyword::Shadows))
        return keywords;
    return keywords | MaterialKeyword::VirtualShadowMaps;
}

MaterialKeyword ContributeVirtualShadowSunMask(RenderServices& services, RenderGraph::RGFrame& frame,
                                               const VirtualShadowSunMaskView& view,
                                               VirtualShadowViewInputs& inputs, MaterialKeyword keywords)
{
    // Transmissive surfaces write no prepass depth: they keep the lookup in the shader.
    if (!Rendering::HasKeyword(keywords, MaterialKeyword::VirtualShadowMaps) || view.TransmissiveOnly ||
        !view.Settings || !SunReadsVirtualPages(services, view.ViewId, *view.Settings))
        return keywords;
    if (!view.Depth.IsValid() || !view.Camera)
        return keywords;
    // One view can draw several world passes in a frame (the recover pass, a probe's cube faces).
    // A pass reuses the mask resolved from its own depth and camera; a new prepass declares a new one.
    const bool matches = inputs.SunMask.IsValid() && inputs.SunMaskDepth.Id == view.Depth.Id &&
                         std::memcmp(inputs.SunMaskViewProjection.Data(), view.Camera->viewProjRel.Data(),
                                     sizeof(float) * 16) == 0;
    if (!matches && !view.CanDeclare)
        return keywords;
    if (!matches)
        DeclareSunMask(services, frame, view, inputs);
    return inputs.SunMask.IsValid() ? keywords | MaterialKeyword::VirtualShadowSunMask : keywords;
}

void ReadVirtualShadowInputs(RenderGraph::RGPassBuilder& pass, const VirtualShadowViewInputs& inputs,
                             MaterialKeyword keywords)
{
    if (!inputs.IsValid())
        return;
    pass.Read(inputs.Atlas, RenderGraph::RGTextureRead::Sampled);
    pass.Read(inputs.PageTable, RenderGraph::RGBufferRead::Storage);
    if (inputs.SunMask.IsValid() && Rendering::HasKeyword(keywords, MaterialKeyword::VirtualShadowSunMask))
        pass.Read(inputs.SunMask, RenderGraph::RGTextureRead::Sampled);
}

void BindVirtualShadowInputs(const RenderGraph::RGFrame& frame, const VirtualShadowViewInputs& inputs,
                             MaterialKeyword keywords, ResolvedPassResources& resources)
{
    const auto atlas = frame.PhysicalTexture(inputs.Atlas);
    const auto table = frame.PhysicalBuffer(inputs.PageTable);
    if (!inputs.IsValid() || !atlas.IsValid() || !table.IsValid())
        return;
    resources.Buffers.push_back({HashStringId("VirtualShadowPageTable"), table, 0, inputs.PageTableBytes});
    resources.Buffers.push_back({HashStringId("VirtualShadowProjections"), inputs.Projections,
                                 inputs.ProjectionsOffset, inputs.ProjectionsBytes});
    resources.Textures.push_back({HashStringId("ge_virtualShadowDepth"), atlas, inputs.DepthSampler});
    resources.Textures.push_back({HashStringId("ge_virtualShadowDepthCompare"), atlas, inputs.CompareSampler});
    // R32_UINT: the keyword is set only after the mask pass imported this physical texture.
    if (Rendering::HasKeyword(keywords, MaterialKeyword::VirtualShadowSunMask) && inputs.SunMask.IsValid())
        resources.Textures.push_back({HashStringId("ge_virtualShadowSunMask"), frame.PhysicalTexture(inputs.SunMask),
                                      inputs.DepthSampler});
}
} // namespace GameEngine::Engine::Renderer
