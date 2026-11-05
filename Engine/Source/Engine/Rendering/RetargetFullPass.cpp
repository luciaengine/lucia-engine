#include "Engine/Rendering/RetargetFullPass.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Rendering/Core/PipelineTypes.h"

namespace GameEngine::Engine::Renderer
{

using namespace GameEngine::Rendering;

struct RetargetFullPass::PreparedDispatch
{
    PipelineHandle Pipeline;
    DescriptorSetHandle Descriptors;
    uint32_t CharacterCount = 0;
};

void RetargetFullPass::Declare(RenderGraph::RGFrame& frame, RenderGraph::RGBuffer atlas)
{
    if (!m_DataStore || m_ComputeShader.empty() || !atlas.IsValid())
        return;
    if (m_DataStore->GetActiveCharacterCount() == 0)
        return;
    auto* dispatch = frame.AllocatePassData<PreparedDispatch>();
    frame.AddPass("RetargetFullCompute",
                  static_cast<int32_t>(GameEngine::Rendering::PassPhase::kEarlySetup),
                  [this, dispatch, atlas](RenderGraph::RGPassBuilder& builder)
                  {
                      builder.Write(atlas);
                      builder.Prepare([this, dispatch, atlas](RenderGraph::RGPrepareContext& context)
                      {
                          return PrepareDispatch(context, context.GetBuffer(atlas), *dispatch);
                      });
                  },
                  [this, dispatch](RenderGraph::RGContext& context)
                  { RecordDispatch(context.Cmd, *dispatch); });
}

bool RetargetFullPass::PrepareDispatch(RenderGraph::RGPrepareContext& context,
                                      BufferHandle atlasBuffer, PreparedDispatch& dispatch)
{
    dispatch = {};
    auto* device = context.GetDevice();
    if (!m_DataStore || m_ComputeShader.empty() || !device)
        return false;
    const uint32_t characterCount = m_DataStore->GetActiveCharacterCount();
    if (characterCount == 0)
        return true;

    // Upload changed characters and any dirty asset buffers.
    m_DataStore->FlushIfDirty();

    // Descriptor set: 4 storage buffers (clip, rig, chars, atlas).
    constexpr uint32_t kStorageBindingCount = 4;
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "RetargetFull_Set0";
    for (uint32_t bindingIndex = 0; bindingIndex < kStorageBindingCount; ++bindingIndex)
    {
        DescriptorBinding binding{};
        binding.binding = bindingIndex;
        binding.type = DescriptorType::StorageBuffer;
        binding.count = 1;
        binding.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(binding);
    }

    if (!m_PipelineId.IsValid())
    {
        ::GameEngine::Rendering::ComputePipelineDesc pipelineDescription{};
        pipelineDescription.ComputeShader = std::make_shared<const std::vector<uint8_t>>(m_ComputeShader);
        pipelineDescription.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
        pipelineDescription.PushConstants.Size      = sizeof(uint32_t);
        pipelineDescription.PushConstants.StageMask = kShaderStageCompute;
        pipelineDescription.DebugName = "RetargetFull_Compute";
        m_PipelineId = device->InternComputePipeline(pipelineDescription);
    }
    const PipelineHandle pipeline = context.PreparePipeline(m_PipelineId);
    if (!pipeline)
        return false;

    auto clipBuffer  = m_DataStore->GetClipBuffer();
    auto rigBuffer   = m_DataStore->GetRigBuffer();
    auto characters = m_DataStore->GetCharsBuffer();

    if (!clipBuffer.IsValid() || !rigBuffer.IsValid() || !characters.IsValid() || !atlasBuffer.IsValid())
        return false;

    DescriptorSetDesc descriptorDescription{};
    descriptorDescription.layout = layout;
    descriptorDescription.transient = true;
    descriptorDescription.debugName = "RetargetFull_DS0";
    const DescriptorSetHandle descriptors = device->CreateDescriptorSet(descriptorDescription);
    if (!descriptors.IsValid())
        return false;

    // Bind uploaded clip, rig and character ranges; the atlas spans all palettes.
    device->UpdateStorageBufferBinding(descriptors, 0, clipBuffer,  0, m_DataStore->GetClipUsedBytes());
    device->UpdateStorageBufferBinding(descriptors, 1, rigBuffer,   0, m_DataStore->GetRigUsedBytes());
    device->UpdateStorageBufferBinding(descriptors, 2, characters, 0, characterCount * sizeof(GPURetargetCharacterParams));
    device->UpdateStorageBufferBinding(descriptors, 3, atlasBuffer, 0, 0);

    dispatch.Pipeline = pipeline;
    dispatch.Descriptors = descriptors;
    dispatch.CharacterCount = characterCount;
    return true;
}

void RetargetFullPass::RecordDispatch(CommandList* commands, const PreparedDispatch& dispatch)
{
    if (!commands || dispatch.CharacterCount == 0)
        return;
    commands->SetPipeline(dispatch.Pipeline);
    commands->BindDescriptorSet(0, dispatch.Descriptors, dispatch.Pipeline);
    commands->SetPushConstants(dispatch.CharacterCount);
    commands->Dispatch(dispatch.CharacterCount, 1, 1);
    m_DataStore->MarkDispatchScheduledThisFrame();
}

} // namespace GameEngine::Engine::Renderer
