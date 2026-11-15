#include "Ocean/OceanPlanarReflection.h"

#include "Ocean/OceanFrameStamp.h"

#include "Engine/Rendering/MirrorCaptureView.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PlanarReflectionMath.h"
#include "Engine/Rendering/RenderServices.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Ocean
{

using namespace ::GameEngine::Rendering;
namespace RG = ::GameEngine::Rendering::RenderGraph;

namespace
{

// Scaled reflection colour: HDR so reflected highlights survive the Fresnel
// blend, with alpha as coverage (0 where nothing drew -> sky-dome fallback).
constexpr TextureFormat kReflectionColorFormat = TextureFormat::R16G16B16A16_FLOAT;

constexpr float kReflectionClipBiasMeters = 0.02f;

} // namespace

OceanPlanarReflection::~OceanPlanarReflection()
{
    // The reflection views and cameras live in the RenderServices registry. The
    // feature (our owner) is destroyed by RenderServices, which still outlives
    // this member's destruction, so releasing here is safe (same as the
    // feature's own producer-subscription Reset in its destructor).
    for (auto& [viewId, mirror] : m_MirrorsByView)
        ReleaseMirror(*mirror);
    m_MirrorsByView.Clear();
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

void OceanPlanarReflection::ReleaseMirror(Mirror& mirror)
{
    if (m_Device && mirror.ColorTexture.IsValid())
        m_Device->DestroyTexture(mirror.ColorTexture);
    mirror.ColorTexture = {};
    if (!m_RenderServices)
        return;
    Engine::Renderer::ReleaseMirrorCaptureView(*m_RenderServices, mirror.Capture);
}

void OceanPlanarReflection::ReleaseIdleMirrors(std::chrono::steady_clock::time_point declarerPrevious)
{
    m_MirrorsByView.EraseIf([this, declarerPrevious](ViewId, std::unique_ptr<Mirror>& mirror) {
        if (!IsOceanViewIdle(mirror->LastDeclared, declarerPrevious))
            return false;
        ReleaseMirror(*mirror);
        return true;
    });
}

const OceanPlanarReflection::Mirror* OceanPlanarReflection::Find(ViewId viewId) const
{
    const auto* mirror = m_MirrorsByView.Find(viewId);
    return mirror ? mirror->get() : nullptr;
}

bool OceanPlanarReflection::IsReflectionView(ViewId viewId) const
{
    if (viewId == 0)
        return false;
    for (const auto& [mainViewId, mirror] : m_MirrorsByView)
        if (mirror->Capture.View == viewId)
            return true;
    return false;
}

bool OceanPlanarReflection::EnsureColorTexture(IDevice& device, Mirror& mirror, uint32_t width,
                                               uint32_t height)
{
    if (width == 0 || height == 0)
        return false;
    if (mirror.ColorTexture.IsValid() && width == mirror.Width && height == mirror.Height)
        return true;

    if (mirror.ColorTexture.IsValid())
        device.DestroyTexture(mirror.ColorTexture);
    mirror.ColorTexture = {};

    TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(kReflectionColorFormat);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    // Rests in ShaderResource so the surface's descriptor-direct sample sees a
    // stable layout and next frame's import starts from a defined state.
    td.initialState = ResourceState::ShaderResource;
    td.debugName = mirror.Name.c_str();
    mirror.ColorTexture = device.CreateTexture(td);
    if (!mirror.ColorTexture.IsValid())
        return false;
    mirror.Width = width;
    mirror.Height = height;

    if (!m_Sampler.IsValid())
        m_Sampler =
            device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_PlanarReflection_Sampler"));
    return m_Sampler.IsValid();
}

bool OceanPlanarReflection::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                                           float seaLevel, float resolutionScale,
                                           uint32_t frameIndex, std::chrono::steady_clock::time_point now)
{
    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return false;
    m_Device = device;
    m_RenderServices = &rs;

    std::unique_ptr<Mirror>& slot = m_MirrorsByView.GetOrInsert(d.View.id);
    if (!slot)
    {
        slot = std::make_unique<Mirror>();
        slot->Name = "Ocean_PlanarReflection_View" + std::to_string(d.View.id);
        slot->DepthName = slot->Name + "_Depth";
    }
    // Boxed: the sweep below moves the map's entries but not this mirror, which it
    // keeps because the mirror is stamped first.
    Mirror& mirror = *slot;
    const std::chrono::steady_clock::time_point previousDeclare = mirror.LastDeclared;
    mirror.LastDeclared = now;
    mirror.ReadyFrameIndex = UINT32_MAX;

    // Copies, not references: d.View may be an element of the view registry's
    // view vector and FindCameraData points into its camera vector. The sweep's
    // releases erase from both, so nothing read after the sweep may come from a
    // reference taken before it.
    const ViewDesc mainView = d.View;
    ReleaseIdleMirrors(previousDeclare);

    const CameraData* mainCamData = rs.Views().FindCameraData(mainView.cameraId);
    if (!mainCamData)
        return false;
    const CameraData mainCam = *mainCamData;
    // Uninitialized camera (e.g. a thumbnail before its first update): inverting a
    // zero projection produces NaNs that poison culling.
    if (mainCam.proj[0][0] == 0.0f && mainCam.proj[1][1] == 0.0f)
        return false;

    // Two-sided: an underwater camera keeps the above-water clip.
    CameraData reflected{};
    if (!Engine::Renderer::MakeMirrorCaptureCamera(mainCam, glm::vec4(0.0f, 1.0f, 0.0f, -seaLevel),
                                                   kReflectionClipBiasMeters, false, reflected))
        return false;
    // The shared capture view requests extraction while the mirror is active; when
    // reflections turn off the request lapses and the view stops costing extraction.
    // A refused bucketer or a stale extraction declines: the surface uses the sky dome.
    Engine::Renderer::UpdateMirrorCaptureView(rs, mainView, reflected, 0xFFFFFFFFu, mirror.Capture);

    const float scale = std::clamp(std::isfinite(resolutionScale) ? resolutionScale : 0.5f,
                                   0.125f, 1.0f);
    const uint32_t w = std::max(1u, static_cast<uint32_t>(
        std::ceil(static_cast<float>(d.RenderWidth) * scale)));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(
        std::ceil(static_cast<float>(d.RenderHeight) * scale)));
    if (!EnsureColorTexture(*device, mirror, w, h))
        return false;

    // The texture exists before the prepare step, so a declined first frame still leaves it bound.
    if (!Engine::Renderer::PrepareMirrorCaptureView(d, mirror.Capture))
        return false;

    // Targets: the feature-owned colour (imported external, so the contributor can
    // bind it descriptor-direct on the surface draw) + a pooled depth (graph-
    // internal only; a transient would trip BuildPassResourcesRG's ge_sceneDepth
    // physical resolve, so use a persistent pool import like the thumbnail path).
    const RG::RGTexture color = d.Frame.ImportExternalTexture(
        mirror.Name.c_str(), mirror.ColorTexture, ResourceState::ShaderResource,
        kReflectionColorFormat);

    TextureDesc dd{};
    dd.width = w;
    dd.height = h;
    dd.depth = 1;
    dd.mipLevels = 1;
    dd.arrayLayers = 1;
    dd.sampleCount = 1;
    dd.format = static_cast<uint32_t>(rs.GetDepthFormat());
    dd.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    dd.debugName = mirror.DepthName.c_str();
    const RG::RGTexture depth = d.Frame.ImportPersistentTexture(mirror.DepthName.c_str(), dd);
    if (!color.IsValid() || !depth.IsValid())
        return false;

    if (!Engine::Renderer::DeclareMirrorCaptureWorld(d, mirror.Capture, color, depth))
        return false;

    // The same-frame consumer (OceanSurface) declares its Sampled read via
    // ImportForSampling + the sampledTextures span — a real RAW edge that both
    // orders the surface after this mirror pass and lands the
    // RenderTarget->ShaderReadOnly transition before it records. MarkOutput
    // sink-anchors the hidden reflection span (bucketer -> world pass) against
    // culling and restores ShaderReadOnly on frames where the surface declines.
    d.Frame.MarkOutput(color, RG::RGImageLayout::ShaderReadOnly);

    mirror.ReadyFrameIndex = frameIndex;
    return true;
}

RG::RGTexture OceanPlanarReflection::ImportForSampling(RG::RGFrame& frame, ViewId viewId,
                                                       uint32_t frameIndex) const
{
    const Mirror* mirror = Find(viewId);
    if (!mirror || !mirror->IsReady(frameIndex))
        return {};
    return frame.ImportExternalTexture(mirror->Name.c_str(), mirror->ColorTexture, ResourceState::ShaderResource,
                                       kReflectionColorFormat);
}

} // namespace GameEngine::Ocean
