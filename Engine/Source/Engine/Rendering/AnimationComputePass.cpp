#include "Engine/Rendering/AnimationComputePass.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Rendering/Core/PipelineTypes.h"

namespace GameEngine::Engine::Renderer
{

using namespace GameEngine::Rendering;

struct AnimationComputePass::PreparedDispatch
{
    PipelineHandle Pipeline;
    DescriptorSetHandle Descriptors;
    struct Parameters
    {
        uint32_t InstanceCount = 0;
        uint32_t GateEnabled = 0;
        uint32_t RuntimeVisibleCap = 0;
    };
    Parameters Constants;
};

void AnimationComputePass::Declare(RenderGraph::RGFrame& frame, RenderGraph::RGBuffer atlas,
                                   RenderGraph::RGBuffer runtimeVisible)
{
    if (!m_DataStore || m_ComputeShader.empty() || !atlas.IsValid())
        return;
    // Preview worlds can append instances after declaration. Preparation reads
    // the final live count before any command list records.
    const bool gate = m_GateEnabled && runtimeVisible.IsValid();
    auto* dispatch = frame.AllocatePassData<PreparedDispatch>();
    frame.AddPass("AnimSkinningCompute",
                  static_cast<int32_t>(GameEngine::Rendering::PassPhase::kEarlySetup),
                  [this, dispatch, atlas, runtimeVisible, gate](RenderGraph::RGPassBuilder& builder)
                  {
                      builder.Prepare([this, dispatch, atlas, runtimeVisible, gate](RenderGraph::RGPrepareContext& context)
                      {
                          return PrepareDispatch(context, context.GetBuffer(atlas),
                                                 gate ? context.GetBuffer(runtimeVisible) : BufferHandle{}, *dispatch);
                      });
                      builder.Write(atlas);
                      if (gate)
                          builder.Read(runtimeVisible); // Orders skinning before the visibility aggregate.
                  },
                  [this, dispatch](RenderGraph::RGContext& context)
                  {
                      RecordDispatch(context.Cmd, *dispatch);
                  });
}

bool AnimationComputePass::PrepareDispatch(RenderGraph::RGPrepareContext& context,
                                           BufferHandle atlasBuffer,
                                           BufferHandle visibilityBuffer,
                                           PreparedDispatch& dispatch)
{
    dispatch = {};
    auto* device = context.GetDevice();
    if (!m_DataStore || m_ComputeShader.empty() || !device)
        return false;

    const uint32_t instanceCount = m_DataStore->GetInstanceCount();
    if (instanceCount == 0)
        return true;

    m_DataStore->FlushInstances();

    // Bindings cover skeletons, clips, instances, the atlas and visibility.
    constexpr uint32_t kStorageBindingCount = 7;
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "AnimCompute_Set0";
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
        pipelineDescription.PushConstants.Size      = sizeof(PreparedDispatch::Parameters);
        pipelineDescription.PushConstants.StageMask = kShaderStageCompute;
        pipelineDescription.DebugName = "AnimSkinning_Compute";
        m_PipelineId = device->InternComputePipeline(pipelineDescription);
    }
    const PipelineHandle pipeline = context.PreparePipeline(m_PipelineId);
    if (!pipeline)
        return false;

    auto skeletonHeaders = m_DataStore->GetSkeletonHeaderBuffer();
    auto skeletonData   = m_DataStore->GetSkeletonDataBuffer();
    auto clipHeaders = m_DataStore->GetClipHeaderBuffer();
    auto clipData   = m_DataStore->GetClipDataBuffer();
    auto instances   = m_DataStore->GetInstanceBuffer();

    if (!skeletonHeaders.IsValid() || !skeletonData.IsValid() ||
        !clipHeaders.IsValid() || !clipData.IsValid() ||
        !instances.IsValid()   || !atlasBuffer.IsValid())
        return false;

    DescriptorSetDesc descriptorDescription{};
    descriptorDescription.layout = layout;
    descriptorDescription.transient = true;
    descriptorDescription.debugName = "AnimCompute_DS0";
    const DescriptorSetHandle descriptors = device->CreateDescriptorSet(descriptorDescription);
    if (!descriptors.IsValid())
        return false;

    // Used-byte ranges keep reads within uploaded data. The atlas uses its
    // whole allocation because instances write distinct palette offsets.
    device->UpdateStorageBufferBinding(descriptors, 0, skeletonHeaders, 0, m_DataStore->GetSkeletonHeaderUsedBytes());
    device->UpdateStorageBufferBinding(descriptors, 1, skeletonData,   0, m_DataStore->GetSkeletonDataUsedBytes());
    device->UpdateStorageBufferBinding(descriptors, 2, clipHeaders, 0, m_DataStore->GetClipHeaderUsedBytes());
    device->UpdateStorageBufferBinding(descriptors, 3, clipData,   0, m_DataStore->GetClipDataUsedBytes());
    device->UpdateStorageBufferBinding(descriptors, 4, instances,   m_DataStore->GetInstanceBufferOffset(),
                                    instanceCount * sizeof(GPUAnimInstance));
    device->UpdateStorageBufferBinding(descriptors, 5, atlasBuffer,      0, 0);

    // The disabled gate never reads binding 6, so the instance buffer supplies
    // a valid descriptor when no visibility buffer is available.
    const bool useGate = visibilityBuffer.IsValid();
    GameEngine::Rendering::BufferHandle visibilityBinding =
        useGate ? visibilityBuffer : instances;
    device->UpdateStorageBufferBinding(descriptors, 6, visibilityBinding, 0, 0);

    dispatch.Pipeline = pipeline;
    dispatch.Descriptors = descriptors;
    dispatch.Constants.InstanceCount = instanceCount;
    dispatch.Constants.GateEnabled = useGate ? 1u : 0u;
    dispatch.Constants.RuntimeVisibleCap = useGate ? m_RuntimeVisibleCap : 0u;
    return true;
}

void AnimationComputePass::RecordDispatch(CommandList* commands, const PreparedDispatch& dispatch)
{
    if (!commands || dispatch.Constants.InstanceCount == 0)
        return;
    commands->SetPipeline(dispatch.Pipeline);
    commands->BindDescriptorSet(0, dispatch.Descriptors, dispatch.Pipeline);
    commands->SetPushConstants(dispatch.Constants);
    commands->Dispatch(dispatch.Constants.InstanceCount, 1, 1);
}

} // namespace GameEngine::Engine::Renderer
