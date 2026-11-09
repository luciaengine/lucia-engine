#pragma once

// CBTUpdateNode — the render-graph seam for the CBT update loop. Declares ONE
// compute pass ("CBT.Update") at kEarlySetup whose exec records the whole
// hand-barriered kernel sequence on ctx.Cmd (the Ocean OceanFFTGenerate pattern,
// plan §5: a single pass owns its internal barriers; only its externally-visible
// output is declared to the graph). The persistent CBT buffers a C3 indexed
// indirect draw consumes (indirect args + the ALL index stream + the per-bisector
// vertex buffer) are imported and declared as Storage writes + MarkOutput so the
// cross-pass barrier forms for the later forward draw. ImportOutputs exposes the
// same RGBuffers (idempotent per frame) so the forward pass can declare its reads
// against them — that declared read is what orders the draw after this pass.

#include "CBTTerrain/CBTActivityReadback.h" // CBTActivitySlot (a defaulted argument)
#include "Rendering/Core/Handle.h" // TextureHandle
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstdint>
#include <functional>

namespace GameEngine::CBTTerrain
{

class CBTInstance;
struct CBTClassifyDesc;
struct CBTFrameParams;

// The CBT buffers imported into the current frame's graph. Import is idempotent
// per (name, handle) within a frame, so the update pass and the later forward
// draw obtain the SAME RGBuffers and the compute->graphics edge forms.
struct CBTUpdateOutputs
{
    Rendering::RenderGraph::RGBuffer IndirectDraw{};
    Rendering::RenderGraph::RGBuffer IndicesAll{};
    Rendering::RenderGraph::RGBuffer IndicesVisible{};
    Rendering::RenderGraph::RGBuffer CurrentVertex{};
    // The prefiltered height chain the draw's vertex modifier reads (CBTResources::GetHeightChainBuffer).
    Rendering::RenderGraph::RGBuffer HeightChain{};
    // The far-field normal chain the surface samples (CBTResources::GetNormalChainTexture).
    Rendering::RenderGraph::RGTexture NormalChain{};
    // The declared update pass, so the caller can order it against producers this module has no
    // business knowing about (the terrain texture upload). Invalid when nothing was declared.
    Rendering::RenderGraph::RGPass Update{};
};

class CBTUpdateNode
{
  public:
    // Import the persistent CBT draw-consumed buffers into `frame`. No side
    // effects beyond the imports; safe to call once per view for the forward
    // reads. Returns empty outputs if the instance is not ready.
    static CBTUpdateOutputs ImportOutputs(Rendering::RenderGraph::RGFrame& frame,
                                          CBTInstance& instance);

    /// Declares one graph update that samples heightSource and publishes the draw buffers.
    /// activity receives counters recorded after the kernels. Declare once per owner frame.
    /// onSubmitted confirms the recorded update after its containing submission succeeds.
    static CBTUpdateOutputs DeclareUpdatePass(Rendering::RenderGraph::RGFrame& frame,
                                              CBTInstance& instance,
                                              const CBTClassifyDesc& classify,
                                              const CBTFrameParams& params, uint32_t frameCounter,
                                              Rendering::TextureHandle heightSource,
                                              const CBTActivitySlot& activity = {},
                                              std::function<void()> onSubmitted = {});
};

} // namespace GameEngine::CBTTerrain
