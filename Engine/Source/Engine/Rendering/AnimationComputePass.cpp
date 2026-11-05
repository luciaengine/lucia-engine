#include "Engine/Rendering/AnimationComputePass.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Rendering/Core/PipelineTypes.h"

namespace GameEngine { namespace Engine { namespace Renderer {

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
                                   RenderGraph::RGBuffer runtimeVisible) {
    if (!m_DataStore || m_ComputeShader.empty() || !atlas.IsValid()) return;
    // No declare-time instance-count gate: thumbnail/preview worlds queue
    // instances AFTER the main world's declaration (old-arm contract), and
    // PrepareDispatch re-checks the live count after declaration — an unused dispatch
    // declaration is effectively free.
    const bool gate = m_GateEnabled && runtimeVisible.IsValid();
    auto* dispatch = frame.AllocatePassData<PreparedDispatch>();
    frame.AddPass("AnimSkinningCompute",
                  static_cast<int32_t>(GameEngine::Rendering::PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p) {
                      p.Prepare([this, dispatch, atlas, runtimeVisible, gate](RenderGraph::RGPrepareContext& context) {
                          return PrepareDispatch(context, context.GetBuffer(atlas),
                                                 gate ? context.GetBuffer(runtimeVisible) : BufferHandle{}, *dispatch);
                      });
                      p.Write(atlas);
                      if (gate)
                          p.Read(runtimeVisible); // BEFORE the aggregate's Write — the WAR edge
                  },
                  [this, dispatch](RenderGraph::RGContext& ctx) {
                      RecordDispatch(ctx.Cmd, *dispatch);
                  });
}

bool AnimationComputePass::PrepareDispatch(RenderGraph::RGPrepareContext& context,
                                           BufferHandle dispatchAtlasBuf,
                                           BufferHandle runtimeVisBuf,
                                           PreparedDispatch& dispatch) {
    auto* dev = context.GetDevice();
    if (!m_DataStore || m_ComputeShader.empty() || !dev) return false;

    const uint32_t instanceCount = m_DataStore->GetInstanceCount();
    if (instanceCount == 0) return true;

    m_DataStore->FlushInstances();

    // Descriptor set layout: 7 storage buffers (added binding 6 for the
    // Phase 6-iii runtime-visibility flag buffer).
    DescriptorSetLayoutDesc set0{};
    set0.debugName = "AnimCompute_Set0";
    for (uint32_t i = 0; i < 7; ++i) {
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
        cd.PushConstants.Size      = sizeof(PreparedDispatch::Parameters);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "AnimSkinning_Compute";
        m_PipelineId = dev->InternComputePipeline(cd);
    }
    PipelineHandle pipe = context.PreparePipeline(m_PipelineId);
    if (!pipe) return false;


    auto skelHeaderBuf = m_DataStore->GetSkeletonHeaderBuffer();
    auto skelDataBuf   = m_DataStore->GetSkeletonDataBuffer();
    auto clipHeaderBuf = m_DataStore->GetClipHeaderBuffer();
    auto clipDataBuf   = m_DataStore->GetClipDataBuffer();
    auto instanceBuf   = m_DataStore->GetInstanceBuffer();
    auto atlasBuf      = dispatchAtlasBuf;

    if (!skelHeaderBuf.IsValid() || !skelDataBuf.IsValid() ||
        !clipHeaderBuf.IsValid() || !clipDataBuf.IsValid() ||
        !instanceBuf.IsValid()   || !atlasBuf.IsValid())
        return false;

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = set0;
    dsDesc.transient = true;
    dsDesc.debugName = "AnimCompute_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
    if (!ds.IsValid()) return false;

    // Bind the exact written byte range so out-of-bound shader reads are caught by
    // robust-buffer-access rather than silently returning stale data from the upload
    // buffer's tail. Execute already returns when instanceCount == 0, which implies
    // EnsureSkeletonUploaded/EnsureClipUploaded ran at least once, so the used-byte
    // accessors are guaranteed > 0 here. Atlas remains whole-buffer — the compute
    // shader writes to per-instance offsets that can span the full allocation.
    dev->UpdateStorageBufferBinding(ds, 0, skelHeaderBuf, 0, m_DataStore->GetSkeletonHeaderUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 1, skelDataBuf,   0, m_DataStore->GetSkeletonDataUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 2, clipHeaderBuf, 0, m_DataStore->GetClipHeaderUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 3, clipDataBuf,   0, m_DataStore->GetClipDataUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 4, instanceBuf,   m_DataStore->GetInstanceBufferOffset(),
                                    instanceCount * sizeof(GPUAnimInstance));
    dev->UpdateStorageBufferBinding(ds, 5, atlasBuf,      0, 0);

    // Phase 6-iii binding 6: runtime-visibility flag buffer. Bind the live
    // buffer when the gate is enabled and the buffer is valid; otherwise
    // fall back to the instance buffer (any valid buffer suffices — the
    // shader's `if (gateEnabled != 0u)` predicate skips the read entirely).
    const bool useGate = runtimeVisBuf.IsValid();
    GameEngine::Rendering::BufferHandle runtimeVisBindBuf =
        useGate ? runtimeVisBuf : instanceBuf;
    dev->UpdateStorageBufferBinding(ds, 6, runtimeVisBindBuf, 0, 0);

    dispatch.Pipeline = pipe;
    dispatch.Descriptors = ds;
    dispatch.Constants.InstanceCount = instanceCount;
    dispatch.Constants.GateEnabled = useGate ? 1u : 0u;
    dispatch.Constants.RuntimeVisibleCap = useGate ? m_RuntimeVisibleCap : 0u;
    return true;
}

void AnimationComputePass::RecordDispatch(CommandList* cl, const PreparedDispatch& dispatch) {
    if (!cl || dispatch.Constants.InstanceCount == 0) return;
    cl->SetPipeline(dispatch.Pipeline);
    cl->BindDescriptorSet(0, dispatch.Descriptors, dispatch.Pipeline);
    cl->SetPushConstants(dispatch.Constants);
    cl->Dispatch(dispatch.Constants.InstanceCount, 1, 1);
}

}}} // namespace GameEngine::Engine::Renderer
