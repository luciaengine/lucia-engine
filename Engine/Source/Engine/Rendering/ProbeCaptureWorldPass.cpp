#include "Engine/Rendering/ProbeCaptureWorldPass.h"

#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/FeatureDeclareContext.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Types/StringId.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;
using Pipeline::Nodes::LightUploadNode;
using Pipeline::Nodes::ViewParamsUploadNode;

namespace
{
using FaceLights = std::array<uint32_t, kMaxProbeCaptureLocalLights>;

// The ClusterBuffer of a grid with one cluster (clustered_lighting.glsl): the header
// (clusters x, y, depth slices, lights per cluster), then the cluster's (first index, light count).
struct alignas(16) SingleClusterGPU
{
    uint32_t Header[4] = {};
    uint32_t Cluster[2] = {};
};

// The LightBuffer indices of the local lights whose reach meets the face frustum, strongest first (the
// world list order), up to kMaxProbeCaptureLocalLights. The LightBuffer holds every buffered light of
// the world in the order of the view's own LightBuffer, so a shadow slot or a spot or area shadow
// index of the view addresses the same light. Returns how many entries of `out` it filled.
uint32_t SelectFaceLights(std::span<const ExtractedLight> lights, const CameraData& camera, FaceLights& out)
{
    Vector4 planes[6];
    ExtractFrustumPlanes(camera.viewProjRel, planes);
    uint32_t count = 0;
    uint32_t bufferIndex = 0;
    for (const ExtractedLight& light : lights)
    {
        if (count == out.size() || bufferIndex == LightUploadNode::kMaxBufferedLights)
            break;
        if (!LightUploadNode::IsBufferedLight(light))
            continue;
        const uint32_t index = bufferIndex++;
        if (light.castsLight == 0)
            continue;
        const float reach = LocalLightReach(light.type, light.areaShape, light.range, light.areaWidth,
                                            light.areaHeight, light.areaRadius);
        if (TestSphereFrustum(ViewRelativePoint(camera, light.positionWS), reach, planes))
            out[count++] = index;
    }
    return count;
}

// Writes the view's LightBuffer, with the point-shadow slots of the face's own shadow assignment, and
// binds it.
bool BindFaceLightBuffer(RenderServices& services, RenderGraph::RGFrame& frame, ViewId viewId, uint64_t worldId,
                         const CameraData& camera)
{
    const LightUploadNode::LightBufferUpload upload = LightUploadNode::UploadLightBuffer(
        frame, services.GetWorldLights(worldId), camera, services.PointShadowSlotOfCluster(viewId, worldId, camera));
    return upload.Valid() &&
           services.BindViewFrameBuffer(frame, viewId, HashStringId("LightBuffer"), upload.Buffer, upload.Offset,
                                        upload.Bytes);
}

// Binds the view's LightUBO without the sun's shadow: the face renders no cascades.
bool BindFaceLightUBO(RenderServices& services, RenderGraph::RGFrame& frame, ViewId viewId, uint64_t worldId)
{
    ForwardLightUBO ubo = services.BuildViewLightUBO(viewId, worldId);
    ubo.uLightColorWorld[3] = 0.0f; // castsShadows of the primary directional light
    const auto alloc = frame.AllocUpload(sizeof(ubo), 256);
    if (!alloc.Valid())
        return false;
    std::memcpy(alloc.Ptr, &ubo, sizeof(ubo));
    return services.BindViewFrameBuffer(frame, viewId, HashStringId("LightUBO"), alloc.Buffer, alloc.Offset,
                                        sizeof(ubo)) &&
           services.BindViewFrameBuffer(frame, viewId, HashStringId("Light"), alloc.Buffer, alloc.Offset,
                                        sizeof(ubo));
}

// Declares the point, spot and area shadow maps of the face's view, through the path a view with no
// shadow-casting sun takes in the shadow node. The face's world pass reads them.
void DeclareFaceShadows(RenderServices& services, RenderGraph::RGFrame& frame, ViewId viewId)
{
    auto* shadows = services.GetFeature<ShadowMapRenderFeature>();
    if (!shadows || !shadows->IsInitialized())
        return;
    services.EmitProducerDepthCommandsForView(viewId);
    const std::string prefix = "ProbeCapture.View" + std::to_string(viewId) + ".";
    const FeatureDeclareContext context = services.MakeFeatureDeclareContext(
        frame, viewId, /*cascadeCount=*/0, shadows->GetConfig().Resolution, /*directionalLightDirWS=*/nullptr,
        [&prefix](const char* suffix) { return prefix + suffix; });
    shadows->Declare(frame, services, context);
}

// Copies `bytes` of `data` into this frame's upload ring and binds them by `name` in the view's passes.
bool UploadAndBind(RenderServices& services, RenderGraph::RGFrame& frame, ViewId viewId, StringId name,
                   const void* data, size_t bytes)
{
    const auto alloc = frame.AllocUpload(bytes, 256);
    if (!alloc.Valid())
        return false;
    std::memcpy(alloc.Ptr, data, bytes);
    return services.BindViewFrameBuffer(frame, viewId, name, alloc.Buffer, alloc.Offset, bytes);
}

// The face camera's ViewParams. Exposure 1 and no jitter: the cube stores absolute radiance.
ViewParamsUBO FaceViewParams(RenderServices& services, ViewId viewId, const CameraData& camera, uint32_t width,
                             uint32_t height)
{
    ViewParamsUBO params{};
    ViewParamsUploadNode::PackCameraFields(camera, services.Views().ResolveCameraRanges(viewId).CullingRange, width,
                                           height, params);
    params.ge_viewRanges[3] = services.GetShaderAnimationTimeSeconds();
    params.ge_exposureParams[0] = 1.0f;
    std::memcpy(params.ge_prevViewProj, params.ge_viewProj, sizeof(params.ge_prevViewProj));
    std::memcpy(params.ge_prevViewProjRel, camera.viewProjRel.Data(), sizeof(params.ge_prevViewProjRel));
    return params;
}
} // namespace

RenderServices::WorldPassRG AddProbeCaptureWorldPass(RenderServices& services, RenderGraph::RGFrame& frame,
                                                     ViewId viewId, const RenderServices::WorldPassTargetsRG& targets,
                                                     uint8_t cullingSlice)
{
    const ViewDesc* view = services.Views().FindViewDesc(viewId);
    if (!view || !targets.Color.IsValid())
        return {};
    const CameraData camera = services.Views().ResolveCameraData(viewId);
    FaceLights lights{};
    const uint32_t lightCount = SelectFaceLights(services.GetWorldLights(view->worldId), camera, lights);
    DeclareFaceShadows(services, frame, viewId);

    // One cluster with every selected light: the shader clamps each pixel to cluster 0 and tests
    // each light's range itself.
    const uint32_t clusterCapacity = std::max(lightCount, 1u);
    SingleClusterGPU cluster{};
    cluster.Header[0] = cluster.Header[1] = cluster.Header[2] = 1u;
    cluster.Header[3] = clusterCapacity;
    cluster.Cluster[1] = lightCount;
    const uint32_t clusterParams[4] = {1u, 1u, 1u, clusterCapacity}; // tile x, tile y, depth slices, lights
    const RenderGraph::RGResourceDesc& target = frame.Graph().ResourceDesc(targets.Color.Id);
    const ViewParamsUBO viewParams = FaceViewParams(services, viewId, camera, target.Width, target.Height);

    const bool bound =
        BindFaceLightBuffer(services, frame, viewId, view->worldId, camera) &&
        BindFaceLightUBO(services, frame, viewId, view->worldId) &&
        UploadAndBind(services, frame, viewId, HashStringId("ClusterBuffer"), &cluster, sizeof(cluster)) &&
        UploadAndBind(services, frame, viewId, HashStringId("LightIndexBuffer"), lights.data(),
                      sizeof(uint32_t) * clusterCapacity) &&
        UploadAndBind(services, frame, viewId, HashStringId("ClusterParams"), clusterParams, sizeof(clusterParams)) &&
        UploadAndBind(services, frame, viewId, HashStringId("ViewParams"), &viewParams, sizeof(viewParams));
    if (!bound)
        return {};
    return services.AddWorldPassForView(frame, viewId, targets,
                                        MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus |
                                            MaterialKeyword::Shadows,
                                        RenderServices::WorldPassDrawScope::All, cullingSlice);
}

} // namespace GameEngine::Engine::Renderer
