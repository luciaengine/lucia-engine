#include "Engine/Rendering/MirrorCaptureView.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;
void ReleaseMirrorCaptureView(RenderServices& services, MirrorCaptureView& capture)
{
    if (capture.View) services.Views().ReleaseView(capture.View);
    if (capture.Camera) services.Views().ReleaseCamera(capture.Camera);
    capture = {};
}
void UpdateMirrorCaptureView(RenderServices& services, const ViewDesc& mainView,
                             const CameraData& reflected, uint32_t mask, MirrorCaptureView& capture)
{
    // mainView can point into the registry's storage; copy before allocating.
    const auto world = mainView.worldId;
    const auto layers = mainView.renderLayerMask & mask;
    capture.SourceView = mainView.id;
    if (!capture.View)
    {
        capture.Camera = services.Views().AllocateCamera("Mirror Capture Camera");
        capture.View = services.Views().AllocateView("Mirror Capture", capture.Camera,
            ViewPurpose::UtilityCapture, ViewParticipation::OnDemand);
        services.Views().SetViewActiveRenderPipeline(capture.View, false);
        services.Views().SetViewWorldPassFlipY(capture.View, true);
    }
    services.Views().SetViewWorldId(capture.View, world);
    services.Views().SetViewRenderLayerMask(capture.View, layers);
    services.Views().SetCameraData(capture.Camera, reflected);
}
bool PrepareMirrorCaptureView(Pipeline::ViewDeclare& d, const MirrorCaptureView& capture)
{
    auto& rs = d.Services;
    if (!capture.View) return false;
    rs.Views().RequestViewFrame(capture.View);
    if (!rs.Views().IsViewExtractionCurrent(capture.View)) return false;
    rs.WriteViewLightBuffer(capture.View);
    rs.BuildWorldBatchKeysForView(capture.View);
    return rs.ScheduleBucketerDispatchesForView(d.Frame, capture.View);
}
bool DeclareMirrorCaptureWorld(Pipeline::ViewDeclare& d, const MirrorCaptureView& capture,
    RenderGraph::RGTexture color, RenderGraph::RGTexture depth, RenderGraph::RGTexture resolve)
{
    auto& rs = d.Services;
    ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearColorValue[3] = 0.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f; // reverse Z
    rs.Views().SetViewClearConfig(capture.View, clear);
    rs.EmitProducerForwardCommandsForView(capture.View, ForwardEmitPurpose::PlanarReflectionCapture);
    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = color;
    targets.Depth = depth;
    targets.Resolve = resolve;
    // No prepass: never let a material sample its own in-flight depth attachment.
    targets.DepthResolved = d.Frame.ImportExternalTexture("MirrorCapture.SceneDepthFallback",
        rs.GetAreaShadowFallbackTexture(), ResourceState::ShaderResource, TextureFormat{});
    if (!targets.DepthResolved.IsValid()) return false;
    auto keywords = MaterialKeyword::Instanced | MaterialKeyword::IBL;
    if (rs.ShareDirectionalShadowsRG(d.Frame, capture.SourceView, capture.View))
        keywords = keywords | MaterialKeyword::Shadows;
    return rs.AddWorldPassForView(d.Frame, capture.View, targets, keywords).Pass.IsValid();
}
}
