#include "Engine/Rendering/SceneColorGrab.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h" // Pipeline::ViewDeclare
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"

#include <utility>

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

SceneColorGrab::SceneColorGrab(std::string sourceResourceName, std::string debugName)
    : m_SourceName(std::move(sourceResourceName)), m_DebugName(std::move(debugName))
{
    m_CopyLayoutName = m_DebugName + ".Copy.Set0";
    m_CopyPipelineName = m_DebugName + ".Copy";
    m_SamplerName = m_DebugName + "_Sampler";
}

SceneColorGrab::~SceneColorGrab()
{
    if (!m_Device)
        return;
    if (m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

void SceneColorGrab::OnDeviceRebuilt()
{
    m_GrabRG = {};
    m_Sampler = {};
    m_ReadyFrameIndex = UINT32_MAX;
}

bool SceneColorGrab::EnsurePipeline(IDevice& device)
{
    if (!m_PipelineLoadAttempted)
    {
        m_PipelineLoadAttempted = true; // one attempt: a missing pkg declines the grab
        if (!RenderGraph::LoadCopyPipelineDesc(device.PreferredShaderSource(),
                                               m_CopyPipelineName.c_str(), m_CopyLayoutName.c_str(),
                                               m_CopyLayout, m_CopyPipeline))
            Logger::Log::Warning(
                "SceneColorGrab({}): copy.shaderpkg unavailable; refraction declines (env-cube fallback).",
                m_DebugName);
    }
    if (!m_CopyPipelineId.IsValid() && !m_CopyPipeline.vertexShader.empty())
        m_CopyPipelineId = PipelineDescTranslator::InternGraphics(device, m_CopyPipeline);
    return m_CopyPipelineId.IsValid();
}

bool SceneColorGrab::DeclareForView(Pipeline::ViewDeclare& d, uint32_t frameIndex)
{
    m_ReadyFrameIndex = UINT32_MAX;
    m_GrabRG = {}; // frame-scoped: last frame's id must never leak into this frame

    auto* device = d.Services.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    // Snapshot the named scene-colour resource. For the transmission lobe this is the
    // post-world resolved "SceneColor" (lit opaque) — the read-after-write edge against
    // the world pass's colour write orders this grab after the world pass regardless of
    // the phase tag.
    const RenderGraph::RGTexture sourceColor = d.ResolveTexture(m_SourceName.c_str());
    if (!sourceColor.IsValid())
        return false;

    // Phase-1 scope: a plain sampler2D copy. If the source is multisampled (the MSAA
    // format-mismatch fallback path leaves SceneColor as the MSAA target), decline rather
    // than resolve — the consumer then falls back to the env cube.
    const RenderGraph::RGResourceDesc scd = d.Frame.Graph().ResourceDesc(sourceColor.Id);
    if (scd.SampleCount > 1)
        return false;
    if (d.RenderWidth == 0 || d.RenderHeight == 0 || !EnsurePipeline(*device))
        return false;
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp(m_SamplerName.c_str()));
    if (!m_Sampler.IsValid())
        return false;

    // Views are declared before any pass executes. A shared backing texture would be
    // resized and destroyed while earlier views still have queued copy and glass passes.
    const std::string grabName = m_DebugName + ".View" + std::to_string(static_cast<uint32_t>(d.View.id));
    TextureDesc desc{};
    desc.width = d.RenderWidth;
    desc.height = d.RenderHeight;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = scd.Format;
    desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    desc.debugName = m_DebugName.c_str();
    const RenderGraph::RGTexture grab = d.Frame.ImportPersistentTexture(grabName.c_str(), desc);
    if (!grab.IsValid())
        return false;

    // Copy: sample the lit-opaque scene into the grab (read source, write grab → the RG
    // inserts the RenderTarget->Sampled edge). MSAA sources declined above, so the
    // sampler2DMS resolve fork is never taken — pass empty handles for it.
    RenderGraph::AddCopyPass(d.Frame, sourceColor, grab, grabName.c_str(),
                             Rendering::PassPhase::kWorldRender, m_CopyPipelineId,
                             GraphicsPipelineId{}, m_CopyLayout, DescriptorSetLayoutDesc{},
                             m_Sampler, d.RenderWidth, d.RenderHeight);

    // Settle the pool-owned snapshot for descriptor-direct sampling and the next frame's
    // import. Each view declares its own copy-to-consumer dependency.
    d.Frame.AddPass(
        (grabName + ".Settle").c_str(), Rendering::PassPhase::kWorldRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(grab, RenderGraph::RGTextureRead::Sampled);
            p.PreventCulling();
        },
        [](RenderGraph::RGContext&) {});

    m_ReadyFrameIndex = frameIndex;
    m_GrabRG = grab;
    return true;
}

} // namespace GameEngine::Engine::Renderer
