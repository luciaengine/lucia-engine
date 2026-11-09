#include "CBTTerrain/CBTUpdateNode.h"

#include "CBTTerrain/CBTActivityReadback.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTResources.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h" // ResourceState
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>

namespace GameEngine::CBTTerrain
{

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;

namespace
{
// One phase past kEarlySetup, which keeps this pass early without delaying the draw: the
// world/depth consumers order after it through its MarkOutput'd buffers, and dependency order
// beats phase. The phase is NOT what puts the terrain texture upload ahead of this pass — that
// upload declares no access this pass shares, so it is its own scheduling component and only an
// explicit ordering edge orders it (the caller adds one; see CBTUpdateOutputs::Update). What the
// phase does buy is placement among the passes this one IS connected to.
constexpr int32_t kCBTUpdatePhase = PassPhase::kEarlySetup + 1;
} // namespace

CBTUpdateOutputs CBTUpdateNode::ImportOutputs(RGFrame& frame, CBTInstance& instance)
{
    CBTUpdateOutputs out{};
    if (!instance.IsReady())
        return out;
    CBTResources& res = instance.GetResources();
    // Import each with its physical byte size: the graph never needs it for scheduling,
    // but a reader that bounds a copy against the resource — the debug server's buffer
    // readback — has no other source for it, and refuses a size-less import.
    const auto importSized = [&frame, &res](const char* name, CBTBinding binding) {
        return frame.ImportExternalBuffer(name, res.GetBuffer(binding),
                                          res.GetBufferByteSize(binding));
    };
    out.IndirectDraw = importSized("CBT.IndirectDraw", CBTBinding::IndirectDraw);
    out.IndicesAll = importSized("CBT.IndicesAll", CBTBinding::IndicesAll);
    out.IndicesVisible = importSized("CBT.IndicesVisible", CBTBinding::IndicesVisible);
    out.CurrentVertex = importSized("CBT.CurrentVertex", CBTBinding::CurrentVertex);
    out.HeightChain = frame.ImportExternalBuffer(
        "CBT.HeightChain", res.GetHeightChainBuffer(),
        static_cast<uint64_t>(res.GetHeightChainWords()) * sizeof(uint32_t));
    // Storage and sampled descriptors share GENERAL throughout the chain's lifetime.
    out.NormalChain = frame.ImportExternalTexture("CBT.NormalChain", res.GetNormalChainTexture(),
                                                  ResourceState::UnorderedAccess, TextureFormat::R16G16_FLOAT,
                                                  std::max(res.GetNormalChainMips(), 1u));
    return out;
}

CBTUpdateOutputs CBTUpdateNode::DeclareUpdatePass(RGFrame& frame, CBTInstance& instance,
                                                  const CBTClassifyDesc& classify,
                                                  const CBTFrameParams& params, uint32_t frameCounter,
                                                  TextureHandle heightSource,
                                                  const CBTActivitySlot& activity)
{
    CBTUpdateOutputs out = ImportOutputs(frame, instance);
    if (!out.IndirectDraw.IsValid())
        return out;

    // C5 read edge: when a real terrain heightmap is bound, import it (at its
    // truthful resting ShaderResource state — bindless-managed, no discard) and
    // declare VertexEval's compute sample of it as a graph read. This keeps the
    // dependency on the height texture RG-visible and hazard-tracked, so a future
    // RG-tracked producer forms the barrier automatically. It does not order this pass
    // against today's upload flush, which writes the texture outside the graph's view —
    // the caller's ordering edge does that. Skipped for the flat 1x1 default (static,
    // no hazard).
    const bool haveHeight = heightSource.IsValid();
    RGTexture heightRG{};
    if (haveHeight)
        heightRG = frame.ImportExternalTexture("CBT.HeightSource", heightSource,
                                               ResourceState::ShaderResource);

    // Publish storage writes for draw arguments, index/vertex streams and the filtered height chain.
    // Forward-pass imports declare their reads, forming graph barriers for both arguments and data.
    // Other CBT buffers remain private to the pass and use explicit internal barriers.
    CBTInstance* inst = &instance;
    const CBTClassifyDesc desc = classify;
    const CBTFrameParams frameParams = params;
    out.Update = frame.AddPass(
        "CBT.Update", kCBTUpdatePhase,
        [&](RGPassBuilder& p)
        {
            p.Write(out.IndirectDraw, RGBufferWrite::Storage);
            p.Write(out.IndicesAll, RGBufferWrite::Storage);
            p.Write(out.IndicesVisible, RGBufferWrite::Storage);
            p.Write(out.CurrentVertex, RGBufferWrite::Storage);
            p.Write(out.HeightChain, RGBufferWrite::Storage);
            p.Write(out.NormalChain, RGTextureWrite::Storage);
            if (haveHeight)
                p.Read(heightRG, RGTextureRead::SampledCompute);
        },
        [inst, desc, frameParams, frameCounter, activity](RGContext& ctx)
        {
            if (!ctx.Cmd)
                return;
            inst->RecordUpdate(*ctx.Cmd, desc, frameParams, frameCounter);
            CBTActivityReadback::Record(*ctx.Cmd,
                                        inst->GetResources().GetBuffer(CBTBinding::WorkQueue),
                                        activity);
        });

    // Anchor culling + form the trailing barriers for the draw consumer.
    frame.MarkOutput(out.IndirectDraw);
    frame.MarkOutput(out.IndicesAll);
    frame.MarkOutput(out.IndicesVisible);
    frame.MarkOutput(out.CurrentVertex);
    frame.MarkOutput(out.HeightChain);
    return out;
}

} // namespace GameEngine::CBTTerrain
