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
// Dependencies order height copies and draw consumers. This phase places the update
// among other connected passes; legacy height copies need an explicit dependency.
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
                                                  const CBTActivitySlot& activity, std::function<void()> onSubmitted)
{
    CBTUpdateOutputs out = ImportOutputs(frame, instance);
    if (!out.IndirectDraw.IsValid())
        return out;

    // The height sample forms a graph dependency with paged cache copies.
    // Legacy texture copies need the caller's explicit ordering edge.
    const bool haveHeight = heightSource.IsValid();
    RGTexture heightRG{};
    if (haveHeight)
        heightRG = frame.ImportExternalTexture("CBT.HeightSource", heightSource,
                                               ResourceState::ShaderResource);

    // Publish draw arguments, geometry and the filtered height chain for graph consumers.
    // The update owns barriers for its internal buffers, which no other pass accesses.
    bool* recorded = frame.AllocatePassData<bool>(false);
    out.Update = frame.AddPass(
        "CBT.Update", kCBTUpdatePhase,
        [&out, haveHeight, heightRG, recorded, &onSubmitted](RGPassBuilder& pass)
        {
            pass.Write(out.IndirectDraw, RGBufferWrite::Storage);
            pass.Write(out.IndicesAll, RGBufferWrite::Storage);
            pass.Write(out.IndicesVisible, RGBufferWrite::Storage);
            pass.Write(out.CurrentVertex, RGBufferWrite::Storage);
            pass.Write(out.HeightChain, RGBufferWrite::Storage);
            pass.Write(out.NormalChain, RGTextureWrite::Storage);
            if (haveHeight)
                pass.Read(heightRG, RGTextureRead::SampledCompute);
            if (onSubmitted)
                pass.OnSubmitted([recorded, submitted = std::move(onSubmitted)]
                {
                    if (*recorded)
                        submitted();
                });
        },
        [instancePointer = &instance, classify, frameParams = params, frameCounter, activity, recorded](RGContext& context)
        {
            if (!context.Cmd)
                return;
            instancePointer->RecordUpdate(*context.Cmd, classify, frameParams, frameCounter);
            CBTActivityReadback::Record(*context.Cmd,
                                        instancePointer->GetResources().GetBuffer(CBTBinding::WorkQueue),
                                        activity);
            *recorded = true;
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
