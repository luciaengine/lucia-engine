#pragma once

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/FrameBufferAllocator.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine { namespace Rendering { class IDevice; } }
namespace GameEngine { namespace Animation { struct SkeletonData; class BoneEvaluationPlan; class AnimationClipData; } }

namespace GameEngine { namespace Engine { namespace Renderer {

// GPU-side skeleton header. Packed std430 — 128 bytes per entry.
// Evaluated bones are the joint closure or an ancestor-closed LOD selection in
// level order, so a parent precedes its children and level L holds bones
// [levelOffsets[L], levelOffsets[L + 1]).
// Offsets are element-index offsets into the flat skeleton data SSBO
// (uint32 elements; floats reinterpret via uintBitsToFloat in GLSL).
struct GPUSkeletonHeader {
    uint32 boneCount;               // evaluated bones, at most GPUAnimationDataStore::kMaxBones
    uint32 skinJointCount;
    uint32 levelCount;
    uint32 parentsOffset;           // index into data SSBO (int32 per bone: parent bone, -1 for a root)
    uint32 restTRSOffset;           // index into data SSBO (float: t3+r4+s3 = 10 per bone)
    uint32 restLocalMatOffset;      // index into data SSBO (float: 16 per bone, col-major)
    uint32 inverseBindOffset;       // index into data SSBO (float: 16 per joint, col-major)
    uint32 jointBonesOffset;        // index into data SSBO (uint32 per joint: its bone, UINT32_MAX for none)
    uint32 levelOffsetsOffset;      // index into data SSBO (uint32, levelCount + 1 entries)
    uint32 boneNodesOffset;         // index into data SSBO (uint32 per bone: its skeleton node, which
                                    // indexes the clip's per-node channel table)
    uint32 _pad0[2];
    float  meshRootInverse[16];     // pre-computed column-major inverse of MeshRootWorld
    uint32 _pad1[4];
};
static_assert(sizeof(GPUSkeletonHeader) == 128, "GPUSkeletonHeader must be 128 bytes for clean GPU indexing");

// GPU-side clip header. Packed std430 — 64 bytes per entry.
struct GPUClipHeader {
    float  duration;
    uint32 channelCount;
    uint32 channelDescOffset;       // index into clip data SSBO
    uint32 keyframeDataOffset;      // index into clip data SSBO
    uint32 boneChannelStartOffset;  // index into clip data SSBO (uint32 per bone)
    uint32 boneChannelCountOffset;  // index into clip data SSBO (uint32 per bone)
    uint32 boneCount;               // skeleton bone count (for channel lookup sizing)
    uint32 _pad[9];
};
static_assert(sizeof(GPUClipHeader) == 64, "GPUClipHeader must be 64 bytes");

// GPU-side channel descriptor. 16 bytes.
struct GPUChannelDesc {
    uint32 boneIndex;
    uint32 pathAndInterp;           // path bits 0-1; interpolation bits 2-3 (linear=0, step=1, cubic=2);
                                   // cubic payload bit 4; words per key in bits 8-31
    uint32 keyframeOffset;          // relative to clip's keyframeDataOffset
    uint32 keyframeCount;
};
static_assert(sizeof(GPUChannelDesc) == 16, "GPUChannelDesc must be 16 bytes");

// GPU-side per-frame animation instance. 32 bytes std430: five words the
// kernel reads, padded to the instance ring's 32-byte alignment.
struct GPUAnimInstance {
    uint32 skeletonGPUIndex;        // index into skeleton header buffer
    uint32 clipGPUIndex;            // index into clip header buffer
    float  time;                    // current playback time
    uint32 atlasOutputOffset;       // pre-allocated offset in palette atlas (mat4 units)
    uint32 runtimeId;               // SkeletonStore runtime id: the kernel reads the
                                    // runtime's visibility flag and skips it when no
                                    // view saw any of its entities this frame
    uint32 _pad[3];
};
static_assert(sizeof(GPUAnimInstance) == 32, "GPUAnimInstance must be 32 bytes");

// Manages persistent GPU SSBOs for skeleton and clip data, plus per-frame
// animation instance accumulation for the compute skinning pass.
class GPUAnimationDataStore {
public:
    // The skinning kernel's limits (animation_skinning.comp names the same
    // two): one workgroup holds a world matrix per joint-closure bone in
    // shared memory and walks the hierarchy one level per barrier.
    static constexpr uint32 kMaxBones = 256;
    static constexpr uint32 kMaxHierarchyLevels = 64;

    // EnsureSkeletonUploaded's answer for a skeleton the kernel cannot take.
    static constexpr uint32 kNotUploaded = UINT32_MAX;

    // True when the skeleton's joint closure fits kMaxBones and
    // kMaxHierarchyLevels; a skeleton over either animates on the CPU path.
    static bool FitsKernelLimits(const GameEngine::Animation::SkeletonData& skeleton,
        const Animation::BoneEvaluationPlan* plan = nullptr);

    bool Initialize(GameEngine::Rendering::IDevice* device);
    void Shutdown();

    // Q6 slice 4 (§8-completion): after an in-place device rebuild the persistent
    // skeleton/clip SSBOs and the per-frame instance ring are dead, yet their handles
    // still read IsValid() — so CreateOrGrowBuffer's grow-path DestroyBuffer would
    // double-free them and Allocate would write a dangling mapped pointer. Zero the
    // SSBO handles + capacities WITHOUT destroy so a forced re-flush recreates them
    // from the surviving CPU mirrors (id->offset caches stay valid -> identical GPU
    // content), and reprovision the instance ring.
    void ReprovisionAfterDeviceRebuild();

    // Call once per frame before any AnimationSystem::Update runs.
    // frameIndex is a device-frame change token, not a ring-slot selector.
    void BeginFrame(uint32_t frameIndex);

    /// Returns a GPU header index, or kNotUploaded for missing topology or kernel overflow.
    /// Uploads are serialized across animation waves and main-thread previews.
    /// Plans stay immutable and owned until skeleton invalidation or Shutdown.
    uint32 EnsureSkeletonUploaded(uint32 skeletonId,
        const SharedPtr<const Animation::BoneEvaluationPlan>& plan = {});
    uint32 EnsureClipUploaded(uint32 clipIndex);

    // Drop the id→header map so the next Ensure* re-packs current CPU
    // rest/IBM/keys. Bake reload keeps skeleton/clip ids stable while the
    // bytes change; leaving the map would keep the pre-bake SSBOs.
    void InvalidateSkeleton(uint32 skeletonId);
    void InvalidateAllClips();

    /// Reserve a complete upload before publishing palette offsets. Serialized with
    /// TryAddInstance and FlushInstances; refusal leaves previously admitted rows valid.
    [[nodiscard]] bool TryReserveInstanceCapacity(uint32 instanceCount);
    /// Reserve capacity and append one row; refusal leaves existing rows unchanged.
    [[nodiscard]] bool TryAddInstance(const GPUAnimInstance& instance);
    uint32 GetInstanceCount() const { return static_cast<uint32>(m_FrameInstances.size()); }

    /// Upload admitted rows after device BeginFrame. Unchanged rows reuse their range;
    /// appended rows receive a disjoint range so earlier prepared bindings stay valid.
    [[nodiscard]] bool FlushInstances();

    // GPU-handled runtime tracking (for SkinningUploadSystem skip).
    void MarkGPUHandled(uint32 runtimeId);
    bool IsGPUHandled(uint32 runtimeId) const;

    // Buffer accessors for compute pass binding.
    GameEngine::Rendering::BufferHandle GetSkeletonHeaderBuffer() const;
    GameEngine::Rendering::BufferHandle GetSkeletonDataBuffer() const;
    GameEngine::Rendering::BufferHandle GetClipHeaderBuffer() const;
    GameEngine::Rendering::BufferHandle GetClipDataBuffer() const;
    GameEngine::Rendering::BufferHandle GetInstanceBuffer() const;
    size_t GetInstanceBufferOffset() const { return m_InstanceBufferOffset; }

    // Byte counts actually populated on the GPU side. Used by the compute pass
    // to bind tight SSBO ranges (keeps robust-buffer-access bounds meaningful
    // instead of always binding the whole multi-MB allocation).
    size_t GetSkeletonHeaderUsedBytes() const { return m_SkelHeaders.size() * sizeof(GPUSkeletonHeader); }
    size_t GetSkeletonDataUsedBytes()   const { return m_SkelData.size() * sizeof(uint32); }
    size_t GetClipHeaderUsedBytes()     const { return m_ClipHeaders.size() * sizeof(GPUClipHeader); }
    size_t GetClipDataUsedBytes()       const { return m_ClipData.size() * sizeof(uint32); }

private:
    struct UploadedClip
    {
        uint32 GPUIndex;
        SharedPtr<const Animation::AnimationClipData> Data;
        float Duration;
    };
    bool PackClip(const UploadedClip& clip, std::vector<GPUClipHeader>& headers, std::vector<uint32>& words) const;
    bool UploadClipBuffers(const std::vector<GPUClipHeader>& headers, const std::vector<uint32>& words);
    struct SkeletonVariant
    {
        uint32 GPUIndex;
        SharedPtr<const Animation::BoneEvaluationPlan> Plan;
    };

    GameEngine::Rendering::IDevice* m_Device = nullptr;

    // Skeleton GPU buffers (persistent, grow-on-demand).
    GameEngine::Rendering::BufferHandle m_SkelHeaderBuf;
    GameEngine::Rendering::BufferHandle m_SkelDataBuf;
    std::vector<GPUSkeletonHeader> m_SkelHeaders;
    std::vector<uint32> m_SkelData;                    // flat packed: ints and floats as uint32
    std::unordered_map<uint32, uint32> m_SkelIdToGPU;  // skeletonId → GPU header index
    FlatMap<std::pair<uint32, uintptr_t>, SkeletonVariant> m_SkelPlanToGPU;
    size_t m_SkelHeaderCapacity = 0;
    size_t m_SkelDataCapacity = 0;
    bool m_SkelDirty = false;

    // Clip GPU buffers (persistent, grow-on-demand).
    GameEngine::Rendering::BufferHandle m_ClipHeaderBuf;
    GameEngine::Rendering::BufferHandle m_ClipDataBuf;
    std::vector<GPUClipHeader> m_ClipHeaders;
    std::vector<uint32> m_ClipData;                    // flat packed channels + keyframes
    FlatMap<uint32, UploadedClip> m_ClipIdToGPU;
    size_t m_ClipHeaderCapacity = 0;
    size_t m_ClipDataCapacity = 0;
    bool m_ClipDirty = false;

    // Reservation can replace a buffer before device BeginFrame waits. The ring
    // therefore has device pacing plus one slot, even though copying happens later.
    GameEngine::Rendering::FrameBufferAllocator m_InstanceAllocator;
    std::vector<GPUAnimInstance> m_FrameInstances;
    size_t m_InstanceBufferOffset = 0;
    size_t m_InstanceAlignment = sizeof(GPUAnimInstance);
    size_t m_AdmittedUploadEndBytes = 0;
    uint32 m_AdmittedInstanceCount = 0;
    uint32 m_UploadedInstanceCount = 0;
    uint32 m_MaxInstanceCount = 0;
    bool m_InstanceFrameActive = false;

    // Per-frame GPU-handled runtime IDs.
    std::unordered_set<uint32> m_GPUHandledRuntimeIds;

    // Tracks whether the compute pass has been scheduled this frame.
    // With multiple views (SceneView, GameView, thumbnails), each BuildFrameGraph
    // would otherwise schedule a redundant dispatch writing the same atlas data.
    bool m_DispatchScheduledThisFrame = false;

public:
    bool IsDispatchScheduledThisFrame() const { return m_DispatchScheduledThisFrame; }
    void MarkDispatchScheduledThisFrame() { m_DispatchScheduledThisFrame = true; }

private:

    // Helper: create or grow a persistent Upload SSBO.
    GameEngine::Rendering::BufferHandle CreateOrGrowBuffer(
        GameEngine::Rendering::BufferHandle existing,
        size_t& currentCapacity,
        size_t requiredSize,
        const char* debugName);

    void ResetInstances();

    // Upload dirty CPU data to GPU buffers.
    void FlushSkeletonBuffers();
    void FlushClipBuffers();
};

}}} // namespace GameEngine::Engine::Renderer
