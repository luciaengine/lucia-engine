#include "Engine/Rendering/RetargetFullPass.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Rendering/Core/PipelineTypes.h"

namespace GameEngine { namespace Engine { namespace Renderer {

using namespace GameEngine::Rendering;

struct RetargetFullPass::PreparedDispatch
{
    PipelineHandle Pipeline;
    DescriptorSetHandle Descriptors;
    uint32_t CharacterCount = 0;
};

void RetargetFullPass::Declare(RenderGraph::RGFrame& frame, RenderGraph::RGBuffer atlas) {
    if (!m_DataStore || m_ComputeShader.empty() || !atlas.IsValid()) return;
    if (m_DataStore->GetActiveCharacterCount() == 0) return;
    auto* dispatch = frame.AllocatePassData<PreparedDispatch>();
    frame.AddPass("RetargetFullCompute",
                  static_cast<int32_t>(GameEngine::Rendering::PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p) {
                      p.Write(atlas);
                      p.Prepare([this, dispatch, atlas](RenderGraph::RGPrepareContext& context) {
                          return PrepareDispatch(context, context.GetBuffer(atlas), *dispatch);
                      });
                  },
                  [this, dispatch](RenderGraph::RGContext& ctx)
                  { RecordDispatch(ctx.Cmd, *dispatch); });
}

bool RetargetFullPass::PrepareDispatch(RenderGraph::RGPrepareContext& context,
                                      BufferHandle dispatchAtlasBuf, PreparedDispatch& dispatch) {
    auto* dev = context.GetDevice();
    if (!m_DataStore || m_ComputeShader.empty() || !dev) return false;
    const uint32_t characterCount = m_DataStore->GetActiveCharacterCount();
    if (characterCount == 0) return true;

    // Flush any per-frame uploads (charsBuf at minimum). Asset-load buffers
    // are only re-uploaded when their dirty bit fires.
    m_DataStore->FlushIfDirty();

    // Descriptor set: 4 storage buffers (clip, rig, chars, atlas).
    DescriptorSetLayoutDesc set0{};
    set0.debugName = "RetargetFull_Set0";
    for (uint32_t i = 0; i < 4; ++i) {
        DescriptorBinding b{};
        b.binding = i;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        set0.bindings.push_back(b);
    }

    if (!m_PipelineId.IsValid())
    {
        ::GameEngine::Rendering::ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(m_ComputeShader);
        cd.DescriptorSetLayouts.push_back(dev->InternDescriptorSetLayout(set0));
        cd.PushConstants.Size      = sizeof(uint32_t);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "RetargetFull_Compute";
        m_PipelineId = dev->InternComputePipeline(cd);
    }
    PipelineHandle pipe = context.PreparePipeline(m_PipelineId);
    if (!pipe) return false;


    auto clipBuf  = m_DataStore->GetClipBuffer();
    auto rigBuf   = m_DataStore->GetRigBuffer();
    auto charsBuf = m_DataStore->GetCharsBuffer();
    auto atlasBuf = dispatchAtlasBuf;

    if (!clipBuf.IsValid() || !rigBuf.IsValid() || !charsBuf.IsValid() || !atlasBuf.IsValid())
        return false;

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = set0;
    dsDesc.transient = true;
    dsDesc.debugName = "RetargetFull_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
    if (!ds.IsValid()) return false;

    // Explicit sizes — UpdateStorageBufferBinding doesn't treat range=0 as
    // VK_WHOLE_SIZE in this engine; passing 0 produces an empty bind that
    // reads as zeros in the shader. Atlas is the one exception we leave at 0
    // because the pass's `ComputeTarget` declaration tracks its full extent.
    dev->UpdateStorageBufferBinding(ds, 0, clipBuf,  0, m_DataStore->GetClipUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 1, rigBuf,   0, m_DataStore->GetRigUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 2, charsBuf, 0, characterCount * sizeof(GPURetargetCharacterParams));
    dev->UpdateStorageBufferBinding(ds, 3, atlasBuf, 0, 0);

    dispatch.Pipeline = pipe;
    dispatch.Descriptors = ds;
    dispatch.CharacterCount = characterCount;
    return true;
}

void RetargetFullPass::RecordDispatch(CommandList* cl, const PreparedDispatch& dispatch) {
    if (!cl || dispatch.CharacterCount == 0) return;
    cl->SetPipeline(dispatch.Pipeline);
    cl->BindDescriptorSet(0, dispatch.Descriptors, dispatch.Pipeline);
    cl->SetPushConstants(dispatch.CharacterCount);
    cl->Dispatch(dispatch.CharacterCount, 1, 1);
    m_DataStore->MarkDispatchScheduledThisFrame();
}

}}} // namespace GameEngine::Engine::Renderer
