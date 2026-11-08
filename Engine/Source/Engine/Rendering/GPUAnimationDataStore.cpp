#include "Engine/Rendering/GPUAnimationDataStore.h"

#include "Animation/BoneEvaluationPlan.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Assets/AnimationClip.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GameEngine { namespace Engine { namespace Renderer {

using BufferHandle = GameEngine::Rendering::BufferHandle;
using BufferDesc   = GameEngine::Rendering::BufferDesc;
using BufferUsage  = GameEngine::Rendering::BufferUsage;
using BufferMemoryUsage = GameEngine::Rendering::BufferMemoryUsage;
using BufferCreateFlags = GameEngine::Rendering::BufferCreateFlags;

// Initial buffer capacities — start small, grow on demand.
// Grow-on-demand is implemented in CreateOrGrowBuffer (doubles capacity).
// Keeping initial small avoids committing ~20 MB of host-visible memory on startup.
static constexpr size_t kSkelHeaderInitial = 8 * sizeof(GPUSkeletonHeader);    // 1 KB (8 skeletons)
static constexpr size_t kSkelDataInitial   = 64u << 10;                        // 64 KB
static constexpr size_t kClipHeaderInitial = 16 * sizeof(GPUClipHeader);       // 1 KB (16 clips)
static constexpr size_t kClipDataInitial   = 256u << 10;                       // 256 KB
static constexpr size_t kInstanceRingSize = 32u << 10;
// Per-slot ceiling. Growth temporarily retains the prior slot buffer as well.
static constexpr size_t kInstanceRingMaxSize = 32u << 20;
// One instance per workgroup; Vulkan and WebGPU guarantee this many groups in X.
static constexpr uint32 kMaxInstanceWorkgroups = 65535u;

// A joint that names no skeleton node, or a node outside the joint closure.
static constexpr uint32 kNoBone = UINT32_MAX;

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

bool GPUAnimationDataStore::Initialize(GameEngine::Rendering::IDevice* device)
{
    if (!device) return false;
    m_Device = device;

    m_SkelHeaderBuf = CreateOrGrowBuffer({}, m_SkelHeaderCapacity, kSkelHeaderInitial, "GPUAnimSkelHeaders");
    m_SkelDataBuf   = CreateOrGrowBuffer({}, m_SkelDataCapacity,   kSkelDataInitial,   "GPUAnimSkelData");
    m_ClipHeaderBuf = CreateOrGrowBuffer({}, m_ClipHeaderCapacity, kClipHeaderInitial,  "GPUAnimClipHeaders");
    m_ClipDataBuf   = CreateOrGrowBuffer({}, m_ClipDataCapacity,   kClipDataInitial,    "GPUAnimClipData");

    const auto& capabilities = device->GetCapabilities();
    const size_t bindingLimit = capabilities.maxStorageBufferBindingSize;
    const size_t maximumCapacity = bindingLimit == 0
        ? kInstanceRingMaxSize : (std::min)(kInstanceRingMaxSize, bindingLimit);
    m_InstanceAlignment = (std::max)(sizeof(GPUAnimInstance), capabilities.minStorageBufferOffsetAlignment);
    if (maximumCapacity < sizeof(GPUAnimInstance)
        || m_InstanceAlignment > maximumCapacity
        || (m_InstanceAlignment & (m_InstanceAlignment - 1u)) != 0)
        return false;
    m_MaxInstanceCount = static_cast<uint32>((std::min)(static_cast<size_t>(kMaxInstanceWorkgroups),
        maximumCapacity / sizeof(GPUAnimInstance)));
    // Reservation grows during update, before the device fence wait.
    if (!m_InstanceAllocator.Initialize(device, (std::min)(kInstanceRingSize, maximumCapacity),
            BufferUsage::Storage, device->GetFramesInFlight() + 1u, m_InstanceAlignment,
            "GPUAnimInstances", maximumCapacity))
        return false;
    ResetInstances();

    return m_SkelHeaderBuf.IsValid() && m_SkelDataBuf.IsValid() &&
           m_ClipHeaderBuf.IsValid() && m_ClipDataBuf.IsValid();
}

void GPUAnimationDataStore::Shutdown()
{
    m_SkelPlanToGPU.Clear();
    if (!m_Device) return;
    m_InstanceAllocator.Shutdown();
    if (m_SkelHeaderBuf.IsValid()) m_Device->DestroyBuffer(m_SkelHeaderBuf);
    if (m_SkelDataBuf.IsValid())   m_Device->DestroyBuffer(m_SkelDataBuf);
    if (m_ClipHeaderBuf.IsValid()) m_Device->DestroyBuffer(m_ClipHeaderBuf);
    if (m_ClipDataBuf.IsValid())   m_Device->DestroyBuffer(m_ClipDataBuf);
    m_Device = nullptr;
    ResetInstances();
}

void GPUAnimationDataStore::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;

    // Zero the dead persistent SSBO handles + capacities. Skipping DestroyBuffer here
    // is not to dodge a double-free (a stale Destroy* is a generational no-op) — it is
    // that CreateOrGrowBuffer's grow path calls DestroyBuffer on `existing` when it
    // IsValid(), and its capacity-check early-return would otherwise reuse the dead
    // handle; zeroing forces a clean recreate. The CPU mirrors + id->GPU-offset caches
    // survive, so re-flushing restores byte-identical content and ids still resolve.
    // Empty-scene asymmetry (intentional): with no skeletons/clips loaded the mirrors
    // are empty, so the dirty flags stay false and the SSBOs are NOT recreated here —
    // they lazily re-allocate on the first EnsureSkeletonUploaded after resume. The
    // instance ring below is always re-created because it is written every frame.
    m_SkelHeaderBuf = m_SkelDataBuf = m_ClipHeaderBuf = m_ClipDataBuf = BufferHandle{};
    m_SkelHeaderCapacity = m_SkelDataCapacity = m_ClipHeaderCapacity = m_ClipDataCapacity = 0;
    m_SkelDirty = !m_SkelHeaders.empty();
    m_ClipDirty = !m_ClipHeaders.empty();
    FlushSkeletonBuffers();
    FlushClipBuffers();

    // The per-frame instance ring's backing buffers are dead too; re-create them so
    // the next BeginFrame/Allocate writes into live mapped memory.
    m_InstanceAllocator.ReprovisionAfterDeviceRebuild();
    ResetInstances();
}

void GPUAnimationDataStore::ResetInstances()
{
    m_FrameInstances.clear();
    m_GPUHandledRuntimeIds.clear();
    m_InstanceBufferOffset = 0;
    m_AdmittedUploadEndBytes = 0;
    m_AdmittedInstanceCount = 0;
    m_UploadedInstanceCount = 0;
    m_InstanceFrameActive = false;
    m_DispatchScheduledThisFrame = false;
}

void GPUAnimationDataStore::BeginFrame(uint32_t frameIndex)
{
    ResetInstances();
    m_InstanceAllocator.BeginFrame(frameIndex);
    m_InstanceFrameActive = true;
}

// ----------------------------------------------------------------
// Buffer management
// ----------------------------------------------------------------

BufferHandle GPUAnimationDataStore::CreateOrGrowBuffer(
    BufferHandle existing, size_t& currentCapacity, size_t requiredSize, const char* debugName)
{
    if (existing.IsValid() && currentCapacity >= requiredSize)
        return existing;

    if (existing.IsValid())
        m_Device->DestroyBuffer(existing);

    size_t newCapacity = (currentCapacity == 0) ? requiredSize : currentCapacity;
    while (newCapacity < requiredSize)
        newCapacity *= 2;

    BufferDesc desc{};
    desc.size = newCapacity;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.persistent = true;
    desc.debugName = debugName;

    currentCapacity = newCapacity;
    return m_Device->CreateBuffer(desc);
}

void GPUAnimationDataStore::FlushSkeletonBuffers()
{
    if (!m_SkelDirty || !m_Device) return;

    const size_t headerBytes = m_SkelHeaders.size() * sizeof(GPUSkeletonHeader);
    const size_t dataBytes   = m_SkelData.size() * sizeof(uint32);

    m_SkelHeaderBuf = CreateOrGrowBuffer(m_SkelHeaderBuf, m_SkelHeaderCapacity, headerBytes, "GPUAnimSkelHeaders");
    m_SkelDataBuf   = CreateOrGrowBuffer(m_SkelDataBuf,   m_SkelDataCapacity,   dataBytes,   "GPUAnimSkelData");

    if (headerBytes > 0)
        m_Device->UpdateBuffer(m_SkelHeaderBuf, 0, headerBytes, m_SkelHeaders.data());
    if (dataBytes > 0)
        m_Device->UpdateBuffer(m_SkelDataBuf, 0, dataBytes, m_SkelData.data());

    m_SkelDirty = false;
}

void GPUAnimationDataStore::FlushClipBuffers()
{
    if (!m_ClipDirty || !m_Device) return;

    const size_t headerBytes = m_ClipHeaders.size() * sizeof(GPUClipHeader);
    const size_t dataBytes   = m_ClipData.size() * sizeof(uint32);

    m_ClipHeaderBuf = CreateOrGrowBuffer(m_ClipHeaderBuf, m_ClipHeaderCapacity, headerBytes, "GPUAnimClipHeaders");
    m_ClipDataBuf   = CreateOrGrowBuffer(m_ClipDataBuf,   m_ClipDataCapacity,   dataBytes,   "GPUAnimClipData");

    if (headerBytes > 0)
        m_Device->UpdateBuffer(m_ClipHeaderBuf, 0, headerBytes, m_ClipHeaders.data());
    if (dataBytes > 0)
        m_Device->UpdateBuffer(m_ClipDataBuf, 0, dataBytes, m_ClipData.data());

    m_ClipDirty = false;
}

// ----------------------------------------------------------------
// Skeleton upload
// ----------------------------------------------------------------

static void PackFloat(std::vector<uint32>& buf, float v)
{
    uint32 bits;
    std::memcpy(&bits, &v, sizeof(float));
    buf.push_back(bits);
}

void GPUAnimationDataStore::InvalidateSkeleton(uint32 skeletonId)
{
    if (skeletonId == 0)
        return;
    m_SkelIdToGPU.erase(skeletonId);
    m_SkelPlanToGPU.EraseIf([skeletonId](const auto& key, const auto&) { return key.first == skeletonId; });
}

void GPUAnimationDataStore::InvalidateAllClips()
{
    m_ClipIdToGPU.clear();
}

bool GPUAnimationDataStore::FitsKernelLimits(const SkeletonData& skeleton, const Animation::BoneEvaluationPlan* plan)
{
    if (plan)
        return plan->IsForSkeleton(skeleton) && plan->SkinNodes().size() <= kMaxBones &&
            !plan->SkinLevelOffsets().empty() && plan->SkinLevelOffsets().size() - 1u <= kMaxHierarchyLevels;
    return skeleton.JointClosure.size() <= kMaxBones
        && skeleton.JointClosureLevelCount() <= kMaxHierarchyLevels;
}

uint32 GPUAnimationDataStore::EnsureSkeletonUploaded(uint32 skeletonId,
    const SharedPtr<const Animation::BoneEvaluationPlan>& plan)
{
    if (!plan)
    {
        const auto it = m_SkelIdToGPU.find(skeletonId);
        if (it != m_SkelIdToGPU.end())
            return it->second;
    }

    auto& skStore = SkeletonStore::Instance();
    const SkeletonData* skel = skStore.Get(skeletonId);
    if (!skel || skel->JointClosure.empty() || (plan && !plan->IsForSkeleton(*skel)))
        return kNotUploaded;
    const auto variantKey = std::make_pair(skeletonId, reinterpret_cast<uintptr_t>(plan.get()));
    if (plan)
    {
        if (const auto* variant = m_SkelPlanToGPU.Find(variantKey))
            return variant->GPUIndex;
    }

    const std::span<const uint32> evaluatedNodes = plan ? plan->SkinNodes() : std::span<const uint32>(skel->JointClosure);
    const std::span<const uint32> levelOffsets = plan ? plan->SkinLevelOffsets()
        : std::span<const uint32>(skel->JointClosureLevelOffsets);
    const uint32 levelCount = levelOffsets.empty() ? 0u : static_cast<uint32>(levelOffsets.size() - 1u);
    if (evaluatedNodes.size() > kMaxBones || levelCount > kMaxHierarchyLevels)
        return kNotUploaded;

    const uint32 nodes = skel->BoneCount;
    const uint32 bones = static_cast<uint32>(evaluatedNodes.size());
    // FBX-imported skeletons commonly leave SkinJointCount==0 and JointNodes
    // empty; mirror the CPU BuildPoseToSkinMatrices fallback (joint j == bone j
    // identity, BoneCount palette entries) so the GPU shader writes a full
    // atlas instead of zero matrices. Same behavior as the
    // RetargetGPUDataStore fallback in commit 5a03eac6. The predicate matches
    // PoseToSkinMatrices.cpp:84 — fall back when the populated joint table
    // doesn't match its declared count, not just when both are zero.
    const bool jointTableValid = (skel->SkinJointCount > 0)
                              && (skel->JointNodes.size() == skel->SkinJointCount);
    const bool synthesizeJoints = !jointTableValid;
    const uint32 joints = synthesizeJoints ? nodes : skel->SkinJointCount;

    // The compact evaluated index of each original node; omitted nodes have none.
    std::vector<uint32> boneOfNode(nodes, kNoBone);
    for (uint32 bone = 0; bone < bones; ++bone)
        boneOfNode[evaluatedNodes[bone]] = bone;

    GPUSkeletonHeader hdr{};
    hdr.boneCount = bones;
    hdr.skinJointCount = joints;
    hdr.levelCount = levelCount;

    // Pre-compute meshRootInverse (column-major).
    {
        const glm::mat4 meshRoot = glm::make_mat4(skel->MeshRootWorld);
        const glm::mat4 inv = glm::inverse(meshRoot);
        std::memcpy(hdr.meshRootInverse, glm::value_ptr(inv), 16 * sizeof(float));
    }

    // Pack data in closure order: parents, restTRS, restLocalMat, boneNodes;
    // then per joint: jointBones, inverseBind; then levelOffsets.

    // Parents (int32 as uint32 reinterpret). The selection is ancestor-closed.
    hdr.parentsOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
    {
        const uint32 node = evaluatedNodes[bone];
        const int32 parentNode = (node < skel->Parent.size()) ? skel->Parent[node] : -1;
        const int32 parent = (parentNode >= 0 && static_cast<uint32>(parentNode) < nodes)
            ? static_cast<int32>(boneOfNode[static_cast<uint32>(parentNode)]) : -1;
        uint32 bits;
        std::memcpy(&bits, &parent, sizeof(int32));
        m_SkelData.push_back(bits);
    }

    // Rest TRS: 10 floats per bone (t3 + r4 + s3).
    hdr.restTRSOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
    {
        const size_t nodeIndex = evaluatedNodes[bone];
        // Translation (3 floats).
        for (int c = 0; c < 3; ++c)
            PackFloat(m_SkelData, (nodeIndex * 3 + c < skel->RestTranslation.size()) ? skel->RestTranslation[nodeIndex * 3 + c] : 0.0f);
        // Rotation (4 floats: x,y,z,w).
        for (int c = 0; c < 4; ++c)
            PackFloat(m_SkelData, (nodeIndex * 4 + c < skel->RestRotation.size()) ? skel->RestRotation[nodeIndex * 4 + c] : (c == 3 ? 1.0f : 0.0f));
        // Scale (3 floats).
        for (int c = 0; c < 3; ++c)
            PackFloat(m_SkelData, (nodeIndex * 3 + c < skel->RestScale.size()) ? skel->RestScale[nodeIndex * 3 + c] : 1.0f);
    }

    // Rest local matrices (16 floats per bone, column-major).
    hdr.restLocalMatOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
    {
        for (int c = 0; c < 16; ++c)
        {
            size_t matrixElementIndex = static_cast<size_t>(evaluatedNodes[bone]) * 16 + c;
            PackFloat(m_SkelData, (matrixElementIndex < skel->RestLocalMatrix.size()) ? skel->RestLocalMatrix[matrixElementIndex] : ((c % 5 == 0) ? 1.0f : 0.0f));
        }
    }

    // Skeleton node per bone (uint32): the clip's channel table is per node.
    hdr.boneNodesOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
        m_SkelData.push_back(evaluatedNodes[bone]);

    // Joint nodes, in skin order. When synthesizing the joint mapping
    // (FBX fallback, see above), JointNodes is empty so the lookup falls
    // through to the identity j → j default.
    std::vector<uint32> jointNodes(joints);
    for (uint32 j = 0; j < joints; ++j)
        jointNodes[j] = (j < skel->JointNodes.size()) ? skel->JointNodes[j] : j;

    // Evaluated ancestor per joint. The correction below restores omitted rest chains.
    hdr.jointBonesOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 j = 0; j < joints; ++j)
    {
        const uint32 node = jointNodes[j];
        const uint32 evaluatedNode = plan && node < nodes ? plan->EvaluatedAncestors()[node] : node;
        m_SkelData.push_back(evaluatedNode < nodes ? boneOfNode[evaluatedNode] : kNoBone);
    }

    // Rest-chain times inverse bind (16 floats per joint, column-major).
    hdr.inverseBindOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 j = 0; j < joints; ++j)
    {
        for (int c = 0; c < 16; ++c)
        {
            if (plan)
            {
                PackFloat(m_SkelData, plan->JointCorrections()[j].Data()[c]);
                continue;
            }
            size_t idx = static_cast<size_t>(jointNodes[j]) * 16 + c;
            PackFloat(m_SkelData, (idx < skel->InverseBind.size()) ? skel->InverseBind[idx] : ((c % 5 == 0) ? 1.0f : 0.0f));
        }
    }

    // Level offsets (uint32, levelCount + 1 entries).
    hdr.levelOffsetsOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 offset : levelOffsets)
        m_SkelData.push_back(offset);

    const uint32 gpuIndex = static_cast<uint32>(m_SkelHeaders.size());
    m_SkelHeaders.push_back(hdr);
    if (plan)
        m_SkelPlanToGPU.InsertOrAssign(variantKey, SkeletonVariant{gpuIndex, plan});
    else
        m_SkelIdToGPU[skeletonId] = gpuIndex;
    m_SkelDirty = true;

    FlushSkeletonBuffers();
    return gpuIndex;
}

// ----------------------------------------------------------------
// Clip upload
// ----------------------------------------------------------------

uint32 GPUAnimationDataStore::EnsureClipUploaded(uint32 clipIndex)
{
    auto it = m_ClipIdToGPU.find(clipIndex);
    if (it != m_ClipIdToGPU.end())
        return it->second;

    auto& clipStore = ClipStore::Instance();
    auto clip = clipStore.Get(clipIndex);
    if (!clip) return 0;

    const auto& channels = clip->GetChannels();
    std::vector<AnimChannel> transformChannels;
    transformChannels.reserve(channels.size());
    for (const auto& ch : channels)
    {
        if (ch.path == AnimPath::Translation || ch.path == AnimPath::Rotation || ch.path == AnimPath::Scale)
            transformChannels.push_back(ch);
    }

    // Determine boneCount from channels (max boneIndex + 1).
    uint32 boneCount = 0;
    for (const auto& ch : transformChannels)
        boneCount = std::max(boneCount, ch.boneIndex + 1);

    // Sort channels by boneIndex, then pack per-bone lookup indices.
    struct ChannelRef { uint32 channelIdx; uint32 boneIndex; };
    std::vector<ChannelRef> sorted;
    sorted.reserve(transformChannels.size());
    for (uint32 i = 0; i < transformChannels.size(); ++i)
        sorted.push_back({i, transformChannels[i].boneIndex});
    std::sort(sorted.begin(), sorted.end(), [](const ChannelRef& a, const ChannelRef& b) {
        return a.boneIndex < b.boneIndex;
    });

    // Build per-bone channel start/count.
    std::vector<uint32> boneChannelStart(boneCount, 0);
    std::vector<uint32> boneChannelCount(boneCount, 0);
    for (uint32 i = 0; i < sorted.size(); ++i)
    {
        uint32 bone = sorted[i].boneIndex;
        if (bone < boneCount)
        {
            if (boneChannelCount[bone] == 0)
                boneChannelStart[bone] = i;
            boneChannelCount[bone]++;
        }
    }

    GPUClipHeader hdr{};
    hdr.duration = clip->GetDuration();
    hdr.channelCount = static_cast<uint32>(transformChannels.size());
    hdr.boneCount = boneCount;

    // Pack channel descriptors (sorted by bone).
    hdr.channelDescOffset = static_cast<uint32>(m_ClipData.size());
    std::vector<uint32> channelKeyframeOffsets(transformChannels.size());
    uint32 keyframeAccum = 0;
    for (uint32 si = 0; si < sorted.size(); ++si)
    {
        const auto& ch = transformChannels[sorted[si].channelIdx];
        GPUChannelDesc desc{};
        desc.boneIndex = ch.boneIndex;
        uint32 pathBits = 0;
        if (ch.path == AnimPath::Translation) pathBits = 0;
        else if (ch.path == AnimPath::Rotation) pathBits = 1;
        else pathBits = 2; // Scale
        uint32 interpBits = 0;
        if (ch.interp == AnimInterp::Step) interpBits = 0;
        else if (ch.interp == AnimInterp::Linear) interpBits = 1;
        else interpBits = 2; // CubicSpline
        desc.pathAndInterp = pathBits | (interpBits << 2);
        desc.keyframeOffset = keyframeAccum;
        desc.keyframeCount = static_cast<uint32>(ch.keys.size());
        channelKeyframeOffsets[sorted[si].channelIdx] = keyframeAccum;
        keyframeAccum += desc.keyframeCount * 28; // 28 floats per keyframe (padded)

        // Pack as 4 uint32.
        m_ClipData.push_back(desc.boneIndex);
        m_ClipData.push_back(desc.pathAndInterp);
        m_ClipData.push_back(desc.keyframeOffset);
        m_ClipData.push_back(desc.keyframeCount);
    }

    // Pack per-bone channel start/count.
    hdr.boneChannelStartOffset = static_cast<uint32>(m_ClipData.size());
    for (uint32 b = 0; b < boneCount; ++b)
        m_ClipData.push_back(boneChannelStart[b]);

    hdr.boneChannelCountOffset = static_cast<uint32>(m_ClipData.size());
    for (uint32 b = 0; b < boneCount; ++b)
        m_ClipData.push_back(boneChannelCount[b]);

    // Pack keyframe data (sorted by bone, 28 floats per keyframe).
    hdr.keyframeDataOffset = static_cast<uint32>(m_ClipData.size());
    for (uint32 si = 0; si < sorted.size(); ++si)
    {
        const auto& ch = transformChannels[sorted[si].channelIdx];
        for (const auto& key : ch.keys)
        {
            PackFloat(m_ClipData, key.time);
            PackFloat(m_ClipData, key.translation[0]);
            PackFloat(m_ClipData, key.translation[1]);
            PackFloat(m_ClipData, key.translation[2]);
            PackFloat(m_ClipData, key.rotation[0]);
            PackFloat(m_ClipData, key.rotation[1]);
            PackFloat(m_ClipData, key.rotation[2]);
            PackFloat(m_ClipData, key.rotation[3]);
            PackFloat(m_ClipData, key.scale[0]);
            PackFloat(m_ClipData, key.scale[1]);
            PackFloat(m_ClipData, key.scale[2]);
            PackFloat(m_ClipData, key.inTangent[0]);
            PackFloat(m_ClipData, key.inTangent[1]);
            PackFloat(m_ClipData, key.inTangent[2]);
            PackFloat(m_ClipData, key.inTangent[3]);
            PackFloat(m_ClipData, key.outTangent[0]);
            PackFloat(m_ClipData, key.outTangent[1]);
            PackFloat(m_ClipData, key.outTangent[2]);
            PackFloat(m_ClipData, key.outTangent[3]);
            PackFloat(m_ClipData, key.inWeight[0]);
            PackFloat(m_ClipData, key.inWeight[1]);
            PackFloat(m_ClipData, key.inWeight[2]);
            PackFloat(m_ClipData, key.inWeight[3]);
            PackFloat(m_ClipData, key.outWeight[0]);
            PackFloat(m_ClipData, key.outWeight[1]);
            PackFloat(m_ClipData, key.outWeight[2]);
            PackFloat(m_ClipData, key.outWeight[3]);
            m_ClipData.push_back(0); // pad to 28 floats (7 x vec4)
        }
    }

    const uint32 gpuIndex = static_cast<uint32>(m_ClipHeaders.size());
    m_ClipHeaders.push_back(hdr);
    m_ClipIdToGPU[clipIndex] = gpuIndex;
    m_ClipDirty = true;

    FlushClipBuffers();
    return gpuIndex;
}

// ----------------------------------------------------------------
// Per-frame instance management
// ----------------------------------------------------------------

bool GPUAnimationDataStore::TryReserveInstanceCapacity(uint32 instanceCount)
{
    if (!m_Device || !m_InstanceFrameActive || instanceCount > m_MaxInstanceCount
        || instanceCount < m_FrameInstances.size())
        return false;
    if (instanceCount == 0)
        return true;
    const size_t bytes = static_cast<size_t>(instanceCount) * sizeof(GPUAnimInstance);
    const size_t usage = m_InstanceAllocator.GetCurrentUsage();
    const size_t capacityLimit = m_InstanceAllocator.GetMaxCapacity();
    if (usage > capacityLimit || m_InstanceAlignment - 1u > capacityLimit - usage)
        return false;
    const size_t offset = GameEngine::Rendering::Utils::AlignUp(usage, m_InstanceAlignment);
    if (bytes > capacityLimit - offset || !m_InstanceAllocator.Reserve(offset + bytes))
        return false;
    m_AdmittedInstanceCount = instanceCount;
    m_AdmittedUploadEndBytes = offset + bytes;
    return true;
}

bool GPUAnimationDataStore::TryAddInstance(const GPUAnimInstance& instance)
{
    if (!TryReserveInstanceCapacity(GetInstanceCount() + 1u))
        return false;
    m_FrameInstances.push_back(instance);
    return true;
}

bool GPUAnimationDataStore::FlushInstances()
{
    const uint32 instanceCount = GetInstanceCount();
    if (instanceCount == 0 || instanceCount == m_UploadedInstanceCount)
        return true;

    const size_t bytes = static_cast<size_t>(instanceCount) * sizeof(GPUAnimInstance);
    // Only this method allocates. Admission reserved this aligned range, and
    // transactional growth preserves its mapping if a later reservation fails.
    const auto allocation = m_InstanceAllocator.Allocate(bytes, m_InstanceAlignment);
    const bool admitted = instanceCount <= m_AdmittedInstanceCount && allocation.IsValid()
        && allocation.offset + bytes <= m_AdmittedUploadEndBytes;
    if (!admitted)
    {
        Logger::Log::Error("GPU animation instance upload violated its capacity reservation");
        assert(admitted);
        return false;
    }
    std::memcpy(allocation.ptr, m_FrameInstances.data(), bytes);
    m_InstanceBufferOffset = allocation.offset;
    m_UploadedInstanceCount = instanceCount;
    m_AdmittedInstanceCount = instanceCount;
    return true;
}

void GPUAnimationDataStore::MarkGPUHandled(uint32 runtimeId)
{
    m_GPUHandledRuntimeIds.insert(runtimeId);
}

bool GPUAnimationDataStore::IsGPUHandled(uint32 runtimeId) const
{
    return m_GPUHandledRuntimeIds.count(runtimeId) > 0;
}

// ----------------------------------------------------------------
// Buffer accessors
// ----------------------------------------------------------------

BufferHandle GPUAnimationDataStore::GetSkeletonHeaderBuffer() const { return m_SkelHeaderBuf; }
BufferHandle GPUAnimationDataStore::GetSkeletonDataBuffer() const   { return m_SkelDataBuf; }
BufferHandle GPUAnimationDataStore::GetClipHeaderBuffer() const     { return m_ClipHeaderBuf; }
BufferHandle GPUAnimationDataStore::GetClipDataBuffer() const       { return m_ClipDataBuf; }
BufferHandle GPUAnimationDataStore::GetInstanceBuffer() const       { return m_InstanceAllocator.GetCurrentBuffer(); }

}}} // namespace GameEngine::Engine::Renderer
