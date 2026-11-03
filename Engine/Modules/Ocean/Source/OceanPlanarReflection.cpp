#include "Ocean/OceanPlanarReflection.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/PlanarReflectionMath.h"

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
    if (m_Device)
    {
        if (m_ColorTexture.IsValid())
            m_Device->DestroyTexture(m_ColorTexture);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
    }
    // The reflection view/camera live in the RenderServices registry. The feature
    // (our owner) is destroyed by RenderServices, which still outlives this
    // member's destruction, so releasing here is safe (same as the feature's own
    // producer-subscription Reset in its destructor).
    if (m_RenderServices)
        Engine::Renderer::ReleaseMirrorCaptureView(*m_RenderServices, m_Capture);
}

bool OceanPlanarReflection::EnsureColorTexture(IDevice& device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return false;
    if (m_ColorTexture.IsValid() && width == m_Width && height == m_Height)
        return true;

    if (m_ColorTexture.IsValid())
        device.DestroyTexture(m_ColorTexture);

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
    td.debugName = "Ocean_PlanarReflection";
    m_ColorTexture = device.CreateTexture(td);
    if (!m_ColorTexture.IsValid())
        return false;
    m_Width = width;
    m_Height = height;

    if (!m_Sampler.IsValid())
        m_Sampler =
            device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_PlanarReflection_Sampler"));
    return m_Sampler.IsValid();
}

bool OceanPlanarReflection::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                                           float seaLevel, float resolutionScale,
                                           uint32_t frameIndex)
{
    m_ReflectionReady = false;
    m_ReadyFrameIndex = UINT32_MAX;

    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    const CameraData* mainCam = rs.Views().FindCameraData(d.View.cameraId);
    if (!mainCam)
        return false;
    // Uninitialized camera (e.g. a thumbnail before its first update): inverting a
    // zero projection produces NaNs that poison culling.
    if (mainCam->proj[0] == 0.0f && mainCam->proj[5] == 0.0f)
        return false;

    CameraData reflected{};
    if (!Engine::Renderer::MakeMirrorCaptureCamera(*mainCam,
        glm::vec4(0, 1, 0, -seaLevel), kReflectionClipBiasMeters, false, reflected))
        return false;
    m_RenderServices = &rs;
    Engine::Renderer::UpdateMirrorCaptureView(rs, d.View, reflected, 0xFFFFFFFFu, m_Capture);
    if (!Engine::Renderer::PrepareMirrorCaptureView(d, m_Capture))
        return false;

    const float scale = std::clamp(std::isfinite(resolutionScale) ? resolutionScale : 0.5f,
                                   0.125f, 1.0f);
    const uint32_t w = std::max(1u, static_cast<uint32_t>(
        std::ceil(static_cast<float>(d.RenderWidth) * scale)));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(
        std::ceil(static_cast<float>(d.RenderHeight) * scale)));
    if (!EnsureColorTexture(*device, w, h))
        return false;

    // Targets: the feature-owned colour (imported external, so the contributor can
    // bind it descriptor-direct on the surface draw) + a pooled depth (graph-
    // internal only; a transient would trip BuildPassResourcesRG's ge_sceneDepth
    // physical resolve, so use a persistent pool import like the thumbnail path).
    const RG::RGTexture color = d.Frame.ImportExternalTexture(
        "Ocean_PlanarReflection", m_ColorTexture, ResourceState::ShaderResource,
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
    dd.debugName = "Ocean_PlanarReflection_Depth";
    const RG::RGTexture depth = d.Frame.ImportPersistentTexture("Ocean_PlanarReflection_Depth", dd);
    if (!color.IsValid() || !depth.IsValid())
        return false;

    if (!Engine::Renderer::DeclareMirrorCaptureWorld(d, m_Capture, color, depth))
        return false;

    // The same-frame consumer (OceanSurface) declares its Sampled read via
    // ImportForSampling + the sampledTextures span — a real RAW edge that both
    // orders the surface after this mirror pass and lands the
    // RenderTarget->ShaderReadOnly transition before it records. MarkOutput
    // sink-anchors the hidden reflection span (bucketer -> world pass) against
    // culling and restores ShaderReadOnly on frames where the surface declines.
    d.Frame.MarkOutput(color, RG::RGImageLayout::ShaderReadOnly);

    m_ReflectionReady = true;
    m_ReadyFrameIndex = frameIndex;
    return true;
}

RG::RGTexture OceanPlanarReflection::ImportForSampling(RG::RGFrame& frame,
                                                       uint32_t frameIndex) const
{
    if (!IsReady(frameIndex) || !m_ColorTexture.IsValid())
        return {};
    return frame.ImportExternalTexture("Ocean_PlanarReflection", m_ColorTexture,
                                       ResourceState::ShaderResource, kReflectionColorFormat);
}

} // namespace GameEngine::Ocean
