#include "Engine/Rendering/PlanarReflectionFeature.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include <algorithm>
#include <cmath>
#include <string>
#include "Rendering/Common/Frustum.h"
#include <glm/gtc/type_ptr.hpp>

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;
namespace RG = ::GameEngine::Rendering::RenderGraph;

PlanarReflectionFeature::~PlanarReflectionFeature()
{
    for (auto& [key, capture] : m_Captures) Release(capture);
}
void PlanarReflectionFeature::Release(CaptureView& capture)
{
    if (!m_Services) return;
    ReleaseMirrorCaptureView(*m_Services, capture);
}

void PlanarReflectionFeature::SyncWorld(RenderServices& services, uint64_t worldId,
                                       std::vector<PlanarReflectionPlane> planes)
{
    std::lock_guard lock(m_Mutex);
    m_Services = &services;
    std::sort(planes.begin(), planes.end(), [](const auto& a, const auto& b)
    {
        if (a.Settings.Priority != b.Settings.Priority)
            return a.Settings.Priority > b.Settings.Priority;
        return a.EntityId < b.EntityId;
    });
    // Keep every authored plane; relevance and the renderer capture budget
    // are resolved for each view in GetPlanes.
    if (planes.empty()) m_Planes.erase(worldId);
    else m_Planes[worldId] = planes;

    for (auto it = m_Captures.begin(); it != m_Captures.end();)
    {
        const auto* mainView = services.Views().FindViewDesc(it->first.MainView);
        if (!mainView || mainView->worldId != it->first.World || (it->first.World == worldId &&
            std::none_of(planes.begin(), planes.end(), [&](const auto& p)
                { return p.EntityId == it->first.Entity; })))
        {
            Release(it->second);
            it = m_Captures.erase(it);
            continue;
        }
        // Update before frame-graph culling, not only when recording the mirror
        // world pass. Otherwise a moving camera would cull using yesterday's plane.
        if (it->first.World == worldId)
        {
            const auto p = std::find_if(planes.begin(), planes.end(), [&](const auto& p)
                { return p.EntityId == it->first.Entity; });
            const auto* camera = services.Views().FindCameraData(mainView->cameraId);
            CameraData reflected{};
            if (camera && p != planes.end() && MakePlanarReflectionCamera(*camera, *p, reflected))
                services.Views().SetCameraData(it->second.Camera, reflected);
            services.Views().SetViewRenderLayerMask(it->second.View,
                mainView->renderLayerMask & p->Settings.CullMask);
        }
        ++it;
    }
}

std::vector<PlanarReflectionPlane> PlanarReflectionFeature::GetPlanes(const ViewDesc& view) const
{
    if (!view.activeRenderPipeline ||
        (view.purpose != ViewPurpose::Game && view.purpose != ViewPurpose::EditorScene))
        return {};
    std::lock_guard lock(m_Mutex);
    const auto it = m_Planes.find(view.worldId);
    if (it == m_Planes.end() || !m_Services) return {};
    const auto* camera = m_Services->Views().FindCameraData(view.cameraId);
    if (!camera) return {};

    // Renderer resource policy: bound captures per view, never authored planes
    // per world. Highest priority wins among visible, front-facing surfaces.
    constexpr size_t kMaxCapturesPerView = 4;
    std::vector<PlanarReflectionPlane> selected;
    selected.reserve(kMaxCapturesPerView);
    for (const auto& plane : it->second)
    {
        CameraData reflected{};
        if (!MakePlanarReflectionCamera(*camera, plane, reflected)) continue;
        // Test the finite unit rectangle in its own space, preserving rotation,
        // scale and shear. The shared frustum test is conservative at corners.
        const Mathematics::Matrix4x4 localToClip(
            glm::make_mat4(camera->proj) * glm::make_mat4(camera->view) *
            glm::inverse(plane.WorldToLocal));
        Mathematics::Vector4 frustum[6];
        ExtractFrustumPlanes(localToClip, frustum);
        if (!TestAabbFrustum({-.5f, 0, -.5f}, {.5f, 0, .5f}, frustum)) continue;
        selected.push_back(plane);
        if (selected.size() == kMaxCapturesPerView) break;
    }
    return selected;
}
bool PlanarReflectionFeature::HasPlanes(const ViewDesc& view) const
{
    return !GetPlanes(view).empty();
}

PlanarReflectionFeature::Capture PlanarReflectionFeature::DeclareCapture(
    Pipeline::ViewDeclare& d, const PlanarReflectionPlane& plane)
{
    std::lock_guard lock(m_Mutex);
    const ViewDesc mainView = d.View;
    auto& rs = d.Services;
    const auto* mainCamera = rs.Views().FindCameraData(mainView.cameraId);
    CameraData reflected{};
    if (!mainCamera || !MakePlanarReflectionCamera(*mainCamera, plane, reflected)) return {};
    m_Services = &rs;
    const CaptureKey key{mainView.worldId, static_cast<uint32_t>(mainView.id), plane.EntityId};
    const std::string name = "PlanarReflection.W" + std::to_string(key.World) + ".View" +
                            std::to_string(key.MainView) + ".Entity" + std::to_string(key.Entity);
    auto& capture = m_Captures[key];
    UpdateMirrorCaptureView(rs, mainView, reflected, plane.Settings.CullMask, capture);
    // First activation waits for extraction. Never present stale content after
    // a hidden view resumes or a different world is loaded.
    if (!PrepareMirrorCaptureView(d, capture)) return {};

    const uint32_t width = std::max(1u, static_cast<uint32_t>(
        std::ceil(d.RenderWidth * plane.Settings.ResolutionScale)));
    const uint32_t height = std::max(1u, static_cast<uint32_t>(
        std::ceil(d.RenderHeight * plane.Settings.ResolutionScale)));
    TextureDesc colorDesc{};
    colorDesc.width = width; colorDesc.height = height;
    colorDesc.depth = 1; colorDesc.arrayLayers = 1; colorDesc.mipLevels = 1; colorDesc.sampleCount = 1;
    colorDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    colorDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    const auto color = d.Frame.ImportPersistentTexture((name + ".Color").c_str(), colorDesc);
    // Reconstruction cannot recover subpixel coverage from a single-sample
    // silhouette. Antialias reduced captures before upscaling; shading still
    // runs at the requested pixel resolution (no sample shading).
    RG::RGTexture rasterColor = color;
    const bool antialiasCapture = plane.Settings.ResolutionScale < 1.0f;
    if (antialiasCapture)
    {
        colorDesc.sampleCount = 4;
        rasterColor = d.Frame.CreateTexture((name + ".ColorMSAA").c_str(), colorDesc);
    }
    TextureDesc depthDesc = colorDesc;
    depthDesc.format = static_cast<uint32_t>(rs.GetDepthFormat());
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    const auto depth = d.Frame.CreateTexture((name + ".Depth").c_str(), depthDesc);
    if (!color.IsValid() || !rasterColor.IsValid() || !depth.IsValid()) return {};

    // The capture has its own camera and BRDF view direction. No planar/SSR
    // pipeline runs for this utility view, so mirrors cannot recursively capture.
    if (!DeclareMirrorCaptureWorld(d, capture, rasterColor, depth,
        antialiasCapture ? color : RG::RGTexture{})) return {};
    d.Frame.MarkPersistentTextureInitialized(color);
    return {color, reflected};
}
} // namespace GameEngine::Engine::Renderer
