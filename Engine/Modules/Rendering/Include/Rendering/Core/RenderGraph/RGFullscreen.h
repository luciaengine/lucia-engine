#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstdint>
#include <span>

namespace GameEngine::Rendering::RenderGraph
{

// Shared fullscreen passes prepare their pipeline and descriptors before recording.
// Callers own shader and blend-state identity and intern the pipeline.

// Resolve graph inputs, raw textures, then fallbacks during preparation.
// A required missing input or missing fallback cancels frame submission.
struct RGFullscreenTextureInput
{
    uint32_t Binding = 0;
    RGTexture Texture{};
    TextureHandle RawTexture{};
    TextureHandle Fallback{};
    SamplerHandle Sampler{};
    SamplerHandle FallbackSampler{}; // invalid => Sampler
    bool Required = false;
};

// One UBO/SSBO input at an offset — RGFrame::AllocUpload allocations bind
// directly. `Declare` (optional) adds a graph Read so a graph-produced buffer
// orders/barriers correctly; the binding itself always uses {Buffer, Offset}.
struct RGFullscreenBufferInput
{
    uint32_t Binding = 0;
    BufferHandle Buffer{};
    uint64_t Offset = 0;
    uint64_t Size = 0;
    bool IsStorage = false;
    RGBuffer Declare{};
};

struct RGFullscreenDesc
{
    const char* Name = nullptr;
    int32_t Phase = 0;
    // Caller-interned pipeline, prepared for the realized attachment formats.
    // The caller selects the multisample-source shader when declaring the pass.
    GraphicsPipelineId Pipeline{};
    DescriptorSetLayoutDesc Layout{};
    std::span<const RGFullscreenTextureInput> Textures{};
    std::span<const RGFullscreenBufferInput> Buffers{};
    RGTexture Target{};
    RGAttachmentOps Ops{}; // Load for blend-composite, DontCare for overwrite
    // Viewport/scissor. 0 => the Target's RGResourceDesc dimensions; pass the
    // view's render size explicitly when the physical target is larger (view
    // rects, letterboxing).
    uint32_t Width = 0;
    uint32_t Height = 0;
};

// Declares the reads/attachment and records the canonical fullscreen body.
// Capacity: at most kMaxFullscreenTextureInputs/kMaxFullscreenBufferInputs
// inputs (asserted). Frame-owned pass state copies the input spans.
inline constexpr uint32_t kMaxFullscreenTextureInputs = 20;
inline constexpr uint32_t kMaxFullscreenBufferInputs = 4;
RGPass AddFullscreenPass(RGFrame& frame, const RGFullscreenDesc& desc);

// The self-read snapshot: copy `source` into `dest` so a composite pass can
// read the scene while writing it. Picks `pipeline` vs `msaaPipeline` (the
// sampler2DMS resolve fork) off source's SampleCount. Layouts must match the
// picked pipeline's set 0: one combined-image-sampler at binding 0.
RGPass AddCopyPass(RGFrame& frame, RGTexture source, RGTexture dest, const char* name,
                   int32_t phase, GraphicsPipelineId pipeline, GraphicsPipelineId msaaPipeline,
                   const DescriptorSetLayoutDesc& layout, const DescriptorSetLayoutDesc& msaaLayout,
                   SamplerHandle sampler, uint32_t width = 0, uint32_t height = 0);

// The non-MSAA pipeline for AddCopyPass: the stock copy program
// (Shaders/copy.shaderpkg) on canonical fullscreen state, with its set 0 layout
// (one combined-image-sampler at binding 0) written to `outLayout`. Returns
// false and leaves both outputs untouched when the package is missing. The
// names are borrowed, not copied, so they must outlive the descs.
bool LoadCopyPipelineDesc(ShaderSourceKind kind, const char* pipelineName, const char* layoutName,
                          DescriptorSetLayoutDesc& outLayout, PipelineDesc& outPipeline);

// Canonical fullscreen pipeline state: fullscreen-triangle vertex fetch (no
// vertex buffers), depth off, cull none, dynamic viewport/scissor, 1 sample.
// The caller fills shaders/layout/blend, then interns via
// PipelineDescTranslator::InternGraphics.
PipelineDesc MakeFullscreenPipelineDesc(const char* debugName);

} // namespace GameEngine::Rendering::RenderGraph
