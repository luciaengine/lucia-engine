#include "Engine/Rendering/ProbeCaptureWorldPass.h"

#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/RenderOrigin.h"

#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Types/StringId.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <span>

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;
using Pipeline::Nodes::LightUploadNode;
using Pipeline::Nodes::ViewParamsUploadNode;

namespace
{
using FaceLights = std::array<const ExtractedLight*, kMaxProbeCaptureLocalLights>;

// The ClusterBuffer of a grid with one cluster (clustered_lighting.glsl): the header
// (clusters x, y, depth slices, lights per cluster), then the cluster's (first index, light count).
struct alignas(16) SingleClusterGPU
{
    uint32_t Header[4] = {};
    uint32_t Cluster[2] = {};
};

// The local lights whose reach meets the face frustum, strongest first (the world list order), up to
// kMaxProbeCaptureLocalLights. Returns how many entries of `out` it filled.
uint32_t SelectFaceLights(std::span<const ExtractedLight> lights, const CameraData& camera, FaceLights& out)
{
    Vector4 planes[6];
    ExtractFrustumPlanes(camera.viewProjRel, planes);
    uint32_t count = 0;
    for (const ExtractedLight& light : lights)
    {
        if (count == out.size())
            break;
        if (!LightUploadNode::IsBufferedLight(light) || light.castsLight == 0)
            continue;
        const float reach = LocalLightReach(light.type, light.areaShape, light.range, light.areaWidth,
                                            light.areaHeight, light.areaRadius);
        if (TestSphereFrustum(ViewRelativePoint(camera, light.positionWS), reach, planes))
            out[count++] = &light;
    }
    return count;
}

// Writes the LightBuffer of the selected lights into this frame's upload ring and binds it.
bool BindFaceLightBuffer(RenderServices& services, RenderGraph::RGFrame& frame, ViewId viewId,
                         const CameraData& camera, const FaceLights& lights, uint32_t lightCount)
{
    // One zeroed record at least: the binding's minimum size counts one element of the light array.
    const size_t bytes = LightUploadNode::kLightBufferHeaderBytes +
                         size_t(std::max(lightCount, 1u)) * sizeof(LightUploadNode::GPULightPacked);
    const auto alloc = frame.AllocUpload(bytes, 256);
    if (!alloc.Valid())
        return false;
    auto* dst = static_cast<std::byte*>(alloc.Ptr);
    std::memset(dst, 0, bytes);
    std::memcpy(dst, &lightCount, sizeof(lightCount));
    auto* records = reinterpret_cast<LightUploadNode::GPULightPacked*>(dst + LightUploadNode::kLightBufferHeaderBytes);
    // The capture has no point-shadow atlas slot: every local light is unshadowed.
    for (uint32_t i = 0; i < lightCount; ++i)
        records[i] = LightUploadNode::PackLight(*lights[i], camera, -1);
    return services.BindViewFrameBuffer(frame, viewId, HashStringId("LightBuffer"), alloc.Buffer, alloc.Offset, bytes);
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

    // One cluster with every selected light: the shader clamps each pixel to cluster 0 and tests
    // each light's range itself.
    const uint32_t clusterCapacity = std::max(lightCount, 1u);
    SingleClusterGPU cluster{};
    cluster.Header[0] = cluster.Header[1] = cluster.Header[2] = 1u;
    cluster.Header[3] = clusterCapacity;
    cluster.Cluster[1] = lightCount;
    std::array<uint32_t, kMaxProbeCaptureLocalLights> indices{};
    std::iota(indices.begin(), indices.end(), 0u);
    const uint32_t clusterParams[4] = {1u, 1u, 1u, clusterCapacity}; // tile x, tile y, depth slices, lights
    const RenderGraph::RGResourceDesc& target = frame.Graph().ResourceDesc(targets.Color.Id);
    const ViewParamsUBO viewParams = FaceViewParams(services, viewId, camera, target.Width, target.Height);

    const bool bound =
        BindFaceLightBuffer(services, frame, viewId, camera, lights, lightCount) &&
        UploadAndBind(services, frame, viewId, HashStringId("ClusterBuffer"), &cluster, sizeof(cluster)) &&
        UploadAndBind(services, frame, viewId, HashStringId("LightIndexBuffer"), indices.data(),
                      sizeof(uint32_t) * clusterCapacity) &&
        UploadAndBind(services, frame, viewId, HashStringId("ClusterParams"), clusterParams, sizeof(clusterParams)) &&
        UploadAndBind(services, frame, viewId, HashStringId("ViewParams"), &viewParams, sizeof(viewParams));
    if (!bound)
        return {};
    return services.AddWorldPassForView(frame, viewId, targets,
                                        MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus,
                                        RenderServices::WorldPassDrawScope::All, cullingSlice);
}

} // namespace GameEngine::Engine::Renderer
