#pragma once

#include "Rendering/Core/Device.h" // PipelineDesc, DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/Pipeline/PipelineIdentifiers.h"     // GraphicsPipelineId
#include "Rendering/Core/RenderGraph/RGFrame.h"     // RGTexture (frame-scoped grab id)

#include <cstdint>
#include <string>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
namespace Pipeline
{
struct ViewDeclare;
} // namespace Pipeline

// Snapshots a scene-colour render target into a sampleable, pool-owned texture per view so a
// later pass can refract / distort it. Generic over the SOURCE resource name so it is
// reusable: the StandardPBR transmission lobe grabs the post-world "SceneColor" (lit
// opaque) for the dedicated transmissive pass to refract; the ocean (later) can grab its
// pre-world "View.Color". Each view has a separate persistent-pool import: the consuming pass
// binds that import's physical descriptor-direct AND declares an in-graph Read against the
// frame-scoped RGTexture this class exposes (GetGrabTextureRG) — that read is the
// copy->consumer RAW edge; without it ordering rides only the scheduler's
// declaration-order tie-break. The transmissive world pass plumbs it through
// WorldPassTargetsRG::SceneGrab.
//
// Phase-1 scope: a single fullscreen copy (Shaders/copy.shaderpkg). When the source is
// multisampled the grab DECLINES (not ready) so the consumer falls back to the env cube,
// rather than carrying an MSAA-resolve path (the ocean keeps its own resolve arm because
// water must always refract).
class SceneColorGrab
{
public:
    SceneColorGrab(std::string sourceResourceName, std::string debugName);
    ~SceneColorGrab();

    // Records the copy (+ settle) passes for this view. Returns true when a grab texture
    // is available to bind/read this frame (false declines: source missing or multisampled).
    bool DeclareForView(Pipeline::ViewDeclare& d, uint32_t frameIndex);

    // After an in-place device rebuild: forget the old sampler and frame import.
    // The render-graph pool recreates its view textures on the next declaration.
    void OnDeviceRebuilt();
    bool IsReady(uint32_t frameIndex) const
    {
        return m_GrabRG.IsValid() && m_ReadyFrameIndex == frameIndex;
    }
    // Frame-scoped render-graph id of this frame's grab import — valid only for the
    // frame DeclareForView last succeeded on. The consuming pass declares its
    // Read(Sampled) against this id (the copy->consumer RAW edge).
    ::GameEngine::Rendering::RenderGraph::RGTexture GetGrabTextureRG() const { return m_GrabRG; }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

private:
    // Lazily build the copy pipeline (stock copy.shaderpkg).
    // Returns false (declines) when the package is unavailable.
    bool EnsurePipeline(::GameEngine::Rendering::IDevice& device);

    std::string m_SourceName;
    std::string m_DebugName;
    // Stable backing for the const char* desc/debug-label fields (composed once in the ctor
    // so the device's debug labels and the interned pipeline desc never dangle).
    std::string m_CopyLayoutName;
    std::string m_CopyPipelineName;
    std::string m_SamplerName;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    // This frame's grab import id; reset at each DeclareForView (frame-scoped).
    ::GameEngine::Rendering::RenderGraph::RGTexture m_GrabRG{};
    ::GameEngine::Rendering::SamplerHandle m_Sampler;
    uint32_t m_ReadyFrameIndex = UINT32_MAX;

    ::GameEngine::Rendering::PipelineDesc m_CopyPipeline{};
    ::GameEngine::Rendering::GraphicsPipelineId m_CopyPipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_CopyLayout{};

    bool m_PipelineLoadAttempted = false;
};

} // namespace GameEngine::Engine::Renderer
