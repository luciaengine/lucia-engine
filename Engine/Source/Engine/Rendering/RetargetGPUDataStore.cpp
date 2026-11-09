#include "Engine/Rendering/RetargetGPUDataStore.h"

#include "Animation/AnimationClip.h"
#include "Animation/Compression/AnimationClipData.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/SkeletonData.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"
#include "Memory/AllocationCategory.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <cstring>

namespace GameEngine { namespace Engine { namespace Renderer {

namespace
{

// GUID -> 64-bit hash key for the per-asset upload tables. GUID is 128 bits;
// folding to 64 with a multiplicative mix is collision-safe for our cardinality
// (a few hundred clips and rig-pairs at most).
uint64_t PackGuid(const GUID& g)
{
    const auto& bytes = g.GetData();
    uint64_t lo = 0, hi = 0;
    std::memcpy(&lo, bytes.data(), 8);
    std::memcpy(&hi, bytes.data() + 8, 8);
    return lo ^ (hi * 0x9E3779B97F4A7C15ull);
}

// Reinterpret a float as its IEEE-754 uint32 bits.
inline uint32_t FloatBits(float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(float));
    return bits;
}

// Append a vec4 (4 floats) to a uint32 buffer. Returns the OFFSET in vec4
// units of the slot just written (matches the shader's std430 vec4[] view).
inline uint32_t PushVec4(std::vector<uint32_t>& buf, float x, float y, float z, float w)
{
    const uint32_t vec4Off = static_cast<uint32_t>(buf.size() / 4);
    buf.push_back(FloatBits(x));
    buf.push_back(FloatBits(y));
    buf.push_back(FloatBits(z));
    buf.push_back(FloatBits(w));
    return vec4Off;
}

inline uint32_t PushQuat(std::vector<uint32_t>& buf, const Mathematics::Quaternion& q)
{
    const auto& g = q.GetGLM();
    return PushVec4(buf, g.x, g.y, g.z, g.w);
}

inline uint32_t PushVec3AsVec4(std::vector<uint32_t>& buf, const Mathematics::Vector3& v)
{
    return PushVec4(buf, v.x, v.y, v.z, 1.0f);
}

// Pack N uint32s into a vec4-aligned buffer with 4 lanes per vec4. Returns
// the vec4 offset of the first 4-pack. Trailing partial pack is zero-padded.
// Shader access: `floatBitsToUint(rig.data[off + (i >> 2u)][i & 3u])`.
template <typename Iter>
inline uint32_t PushU32Packed(std::vector<uint32_t>& buf, Iter begin, Iter end)
{
    const uint32_t firstVec4 = static_cast<uint32_t>(buf.size() / 4);
    for (auto it = begin; it != end; ++it) buf.push_back(static_cast<uint32_t>(*it));
    while (buf.size() % 4 != 0) buf.push_back(0);
    return firstVec4;
}

Mathematics::Quaternion SampleRotation(const Animation::AnimationClipData& data,
    const Animation::AnimationClipData::Track& track, float time)
{
    Mathematics::Vector4 value;
    return data.Sample(track, time, value)
        ? Mathematics::Quaternion(value.w, value.x, value.y, value.z) : Mathematics::Quaternion::Identity();
}

Mathematics::Vector3 SampleTranslation(const Animation::AnimationClipData& data,
    const Animation::AnimationClipData::Track& track, float time)
{
    Mathematics::Vector4 value;
    return data.Sample(track, time, value) ? Mathematics::Vector3(value.x, value.y, value.z)
        : Mathematics::Vector3();
}

} // namespace

bool RetargetGPUDataStore::Initialize(GameEngine::Rendering::IDevice* device)
{
    if (!device) return false;
    m_Device = device;

    // Buffers are created lazily on first upload via CreateOrGrowBuffer. Zero
    // initial capacity keeps the store cheap when no retargeted characters
    // are spawned.
    m_ClipBuf = {};
    m_RigBuf = {};
    m_CharsBuf = {};
    m_ClipBufCapacity = 0;
    m_RigBufCapacity = 0;
    m_CharsBufCapacity = 0;

    UpdateCategoryBytes();
    return true;
}

void RetargetGPUDataStore::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;

    // Drop the dead SSBO handles WITHOUT DestroyBuffer (CreateOrGrowBuffer's grow-path
    // double-free) and zero the capacities so they recreate. The CPU mirrors + the
    // clip/rig/char id->offset caches survive, so re-flushing produces identical GPU
    // content and cached ids keep resolving.
    m_ClipBuf  = {};
    m_RigBuf   = {};
    m_CharsBuf = {};
    m_ClipBufCapacity  = 0;
    m_RigBufCapacity   = 0;
    m_CharsBufCapacity = 0;
    m_ClipDirty  = !m_ClipData.empty();
    m_RigDirty   = !m_RigData.empty();
    m_CharsDirty = !m_Chars.empty();
    FlushIfDirty();
}

void RetargetGPUDataStore::Shutdown()
{
    // Detach asset-event subscriptions before tearing buffers / tables down so
    // a late-arriving event from another thread doesn't reach into a half-
    // shutdown state.
    m_ClipUnloadWatcher.Reset();
    m_MapUnloadWatcher.Reset();

    if (m_Device)
    {
        if (m_ClipBuf.IsValid())  m_Device->DestroyBuffer(m_ClipBuf);
        if (m_RigBuf.IsValid())   m_Device->DestroyBuffer(m_RigBuf);
        if (m_CharsBuf.IsValid()) m_Device->DestroyBuffer(m_CharsBuf);
    }
    m_ClipBuf = {};
    m_RigBuf = {};
    m_CharsBuf = {};
    m_ClipData.clear();
    m_RigData.clear();
    m_Chars.clear();
    m_Clips.Clear();
    m_NextClipSlot = 0u;
    m_RigGuidToHeaderIdx.clear();
    m_ClipBufCapacity = 0;
    m_RigBufCapacity = 0;
    m_CharsBufCapacity = 0;
    m_ClipDirty = m_RigDirty = m_CharsDirty = false;
    m_ActiveCharCount = 0;
    m_DispatchScheduledThisFrame = false;
    m_Device = nullptr;
    UpdateCategoryBytes();
}

void RetargetGPUDataStore::BeginFrame(uint32_t /*frameIndex*/)
{
    m_DispatchScheduledThisFrame = false;
    m_HandledRuntimes.clear();

    // Drain queued asset-unload releases (AttachAssetEvents callbacks).
    // Swap-and-process so the release calls run outside the queue lock — the
    // dispatcher callback only takes m_PendingReleaseMutex, never the table
    // mutex (the data-store tables aren't locked; they're single-threaded
    // owned by this frame-start site + the system update wave).
    std::vector<GUID> clipReleases;
    std::vector<GUID> mapReleases;
    {
        std::lock_guard<std::mutex> lk(m_PendingReleaseMutex);
        clipReleases.swap(m_PendingClipReleases);
        mapReleases.swap(m_PendingMapReleases);
    }
    for (const auto& g : clipReleases) ReleaseClip(g);
    for (const auto& g : mapReleases)  ReleaseRigPair(g);

    // Clear per-frame character params so each ticked HumanoidRetargetSystem
    // instance (main scene + each thumbnail world) starts from an empty list
    // and contributes additively via AppendCharacterParams. Without this
    // clear, the previous frame's characters would leak forward; without
    // additive semantics on the append, the second instance would wipe the
    // first (the staggering bug observed when both scene and thumbnail
    // animate the same skeleton).
    m_Chars.clear();
    m_ActiveCharCount = 0;
    m_CharsDirty = true;
    if (m_Clips.Empty())
    {
        std::vector<uint32_t>().swap(m_ClipData);
        m_NextClipSlot = 0u;
        if (m_Device && m_ClipBuf.IsValid())
            m_Device->DestroyBuffer(m_ClipBuf);
        m_ClipBuf = {};
        m_ClipBufCapacity = 0u;
        m_ClipDirty = false;
        UpdateCategoryBytes();
    }
}

bool RetargetGPUDataStore::PackClip(const UploadedClip& clip, std::vector<uint32_t>& words) const
{
    struct BoneTracks
    {
        const Animation::AnimationClipData::Track* Rotation = nullptr;
        const Animation::AnimationClipData::Track* Translation = nullptr;
    };
    FlatMap<uint32, BoneTracks> bones;
    for (const auto& track : clip.Data->Tracks())
    {
        if (track.Path != Animation::AnimPath::Rotation && track.Path != Animation::AnimPath::Translation)
            continue;
        constexpr uint32 kMaximumClipNodes = 1u << 20u;
        if (track.BoneIndex >= kMaximumClipNodes)
            return false;
        auto& bone = bones.GetOrInsert(track.BoneIndex);
        if (track.Path == Animation::AnimPath::Rotation && !bone.Rotation)
            bone.Rotation = &track;
        if (track.Path == Animation::AnimPath::Translation && !bone.Translation)
            bone.Translation = &track;
    }
    if (bones.Empty() || !std::isfinite(clip.Duration) || clip.Duration < 0.0f)
        return false;
    const float duration = std::max(1.0f / kRetargetClipSampleRate, clip.Duration);
    const double samples = std::max(2.0, std::ceil(static_cast<double>(duration) * kRetargetClipSampleRate) + 1.0);
    uint32 translationCount = 0u;
    for (const auto& entry : bones)
        translationCount += entry.second.Translation ? 1u : 0u;
    const size_t bindingLimit = m_Device->GetCapabilities().maxStorageBufferBindingSize;
    const size_t maximumBytes = bindingLimit != 0u ? bindingLimit : 256u * 1024u * 1024u;
    const size_t sampleStride = bones.Size() + translationCount;
    const double requiredBytes = static_cast<double>(words.size() * sizeof(uint32_t)) +
        bones.Size() * sizeof(GPURetargetClipBoneRec) + samples * sampleStride * 16u;
    if (requiredBytes > maximumBytes || samples > UINT32_MAX)
        return false;
    const uint32 sampleCount = static_cast<uint32>(samples);
    GPURetargetClipHeader header{};
    header.firstBoneVec4 = static_cast<uint32>(words.size() / 4u);
    header.firstSampleVec4 = header.firstBoneVec4 + static_cast<uint32>(bones.Size());
    header.boneCount = static_cast<uint32>(bones.Size());
    header.transCount = translationCount;
    header.sampleCount = sampleCount;
    header.duration = duration;
    uint32 boneSlot = 0u;
    uint32 translationSlot = 0u;
    for (const auto& [boneIndex, tracks] : bones)
    {
        const GPURetargetClipBoneRec record{boneIndex, boneSlot++,
            tracks.Translation ? translationSlot++ : kRetargetInvalidIndex, 0u};
        const auto packed = std::bit_cast<std::array<uint32, 4u>>(record);
        words.insert(words.end(), packed.begin(), packed.end());
    }
    for (uint32 sample = 0u; sample < sampleCount; ++sample)
    {
        const float time = static_cast<float>(sample) / kRetargetClipSampleRate;
        for (const auto& entry : bones)
        {
            const auto* track = entry.second.Rotation;
            PushQuat(words, track ? SampleRotation(*clip.Data, *track, time) : Mathematics::Quaternion::Identity());
        }
        for (const auto& entry : bones)
        {
            if (const auto* track = entry.second.Translation)
                PushVec3AsVec4(words, SampleTranslation(*clip.Data, *track, time));
        }
    }
    const auto packed = std::bit_cast<std::array<uint32, 8u>>(header);
    std::copy(packed.begin(), packed.end(), words.begin() + static_cast<size_t>(clip.Slot) * packed.size());
    return true;
}

bool RetargetGPUDataStore::UploadClipData(const std::vector<uint32_t>& words)
{
    GameEngine::Rendering::BufferDesc description{};
    description.size = std::max<size_t>(words.size() * sizeof(uint32_t), 16u);
    description.usage = static_cast<uint32_t>(GameEngine::Rendering::BufferUsage::Storage |
        GameEngine::Rendering::BufferUsage::TransferDst);
    description.memoryUsage = GameEngine::Rendering::BufferMemoryUsage::Upload;
    description.flags = GameEngine::Rendering::BufferCreateFlags::PersistentlyMapped;
    description.persistent = true;
    description.debugName = "RetargetClipBuf";
    const auto buffer = m_Device->CreateBuffer(description);
    if (!buffer.IsValid())
        return false;
    void* mapped = m_Device->MapBuffer(buffer);
    if (!mapped)
    {
        m_Device->DestroyBuffer(buffer);
        return false;
    }
    m_Device->UnmapBuffer(buffer);
    if (!words.empty())
        m_Device->UpdateBuffer(buffer, 0u, words.size() * sizeof(uint32_t), words.data());
    const auto previous = m_ClipBuf;
    m_ClipBuf = buffer;
    m_ClipBufCapacity = description.size;
    if (previous.IsValid())
        m_Device->DestroyBuffer(previous);
    return true;
}

uint32_t RetargetGPUDataStore::EnsureClipUploaded(const GUID& guid, const Animation::AnimationClip& clip)
{
    if (!m_Device)
        return kRetargetInvalidIndex;
    const auto data = clip.GetRuntimeData();
    if (!data)
        return kRetargetInvalidIndex;
    const uint64 key = PackGuid(guid);
    const auto* previous = m_Clips.Find(key);
    if (previous && previous->Data == data && previous->Duration == clip.GetDuration() && m_ClipBuf.IsValid())
        return previous->Slot;
    const uint32 slot = previous ? previous->Slot : m_NextClipSlot;
    constexpr size_t kHeaderWords = sizeof(GPURetargetClipHeader) / sizeof(uint32_t);
    const size_t slotCount = std::max<size_t>(m_NextClipSlot, static_cast<size_t>(slot) + 1u);
    const size_t bindingLimit = m_Device->GetCapabilities().maxStorageBufferBindingSize;
    if (slotCount > UINT32_MAX / kHeaderWords ||
        (bindingLimit != 0u && slotCount * sizeof(GPURetargetClipHeader) > bindingLimit))
        return kRetargetInvalidIndex;
    auto candidates = m_Clips;
    candidates.InsertOrAssign(key, UploadedClip{slot, clip.GetDuration(), data});
    // Logical header slots stay fixed while every payload offset is rebuilt.
    std::vector<uint32_t> words(slotCount * kHeaderWords, 0u);
    for (const auto& entry : candidates)
    {
        if (!PackClip(entry.second, words))
            return kRetargetInvalidIndex;
    }
    if (!UploadClipData(words))
        return kRetargetInvalidIndex;
    m_Clips = std::move(candidates);
    m_NextClipSlot = static_cast<uint32>(slotCount);
    m_ClipData = std::move(words);
    m_ClipDirty = false;
    UpdateCategoryBytes();
    return slot;
}

uint32_t RetargetGPUDataStore::EnsureRigPairUploaded(const GUID& mapGuid,
                                                     const Animation::RetargetNode& built,
                                                     const Animation::SkeletonData& /*srcSkel*/,
                                                     const Animation::SkeletonData& tgtSkel,
                                                     uint32_t skinJointCount)
{
    const uint64_t key = PackGuid(mapGuid);
    if (auto it = m_RigGuidToHeaderIdx.find(key); it != m_RigGuidToHeaderIdx.end())
        return it->second;

    Animation::RetargetNode::GPUExport ex;
    built.ExportForGPU(ex);
    if (ex.SourceBoneCount == 0 || ex.TargetBoneCount == 0) return kRetargetInvalidIndex;

    // Header reservation. GPURigPairHeader is 80 bytes = 5 vec4. We push it
    // last (after data is written and we know all the offsets), so reserve
    // the slot first by inserting placeholder zeros.
    constexpr uint32_t kHeaderVec4Size = sizeof(GPURigPairHeader) / 16;
    static_assert(sizeof(GPURigPairHeader) == 80);
    const uint32_t headerVec4Off = static_cast<uint32_t>(m_RigData.size() / 4);
    for (uint32_t i = 0; i < kHeaderVec4Size * 4; ++i) m_RigData.push_back(0);

    GPURigPairHeader hdr{};
    hdr.srcBoneCount   = ex.SourceBoneCount;
    hdr.tgtBoneCount   = ex.TargetBoneCount;
    hdr.skinJointCount = skinJointCount;

    // Helper: convert int32 (with -1 = no parent) to packed uint (with
    // kRetargetInvalidIndex sentinel).
    auto sentinelize = [](int32 v) -> uint32_t {
        return (v < 0) ? kRetargetInvalidIndex : static_cast<uint32_t>(v);
    };

    // Source skeleton.
    {
        std::vector<uint32_t> packedParents;
        packedParents.reserve(ex.SourceBoneCount);
        for (int32 p : ex.SourceParent) packedParents.push_back(sentinelize(p));
        hdr.srcParentIndices = PushU32Packed(m_RigData, packedParents.begin(), packedParents.end());
        hdr.srcDepthSortedTopo = PushU32Packed(m_RigData, ex.SourceTopo.begin(), ex.SourceTopo.end());
        const uint32_t restRotOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& q : ex.SourceRestRot) PushQuat(m_RigData, q);
        hdr.srcRestRot = restRotOff;
    }

    // Target skeleton.
    {
        std::vector<uint32_t> packedParents;
        packedParents.reserve(ex.TargetBoneCount);
        for (int32 p : ex.TargetParent) packedParents.push_back(sentinelize(p));
        hdr.tgtParentIndices = PushU32Packed(m_RigData, packedParents.begin(), packedParents.end());
        hdr.tgtDepthSortedTopo = PushU32Packed(m_RigData, ex.TargetTopo.begin(), ex.TargetTopo.end());

        const uint32_t bindLocalOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& q : ex.TargetBindLocal) PushQuat(m_RigData, q);
        hdr.tgtBindLocal = bindLocalOff;

        const uint32_t restPosOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& v : ex.TargetRestPos) PushVec3AsVec4(m_RigData, v);
        hdr.tgtRestPos = restPosOff;
    }

    // Per-target-bone canonical routing.
    hdr.tgtSrcIdx = PushU32Packed(m_RigData, ex.TargetSourceIdx.begin(), ex.TargetSourceIdx.end());
    {
        const uint32_t bindCorrOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& q : ex.TargetBindCorrection) PushQuat(m_RigData, q);
        hdr.tgtBindCorrection = bindCorrOff;
    }

    // Translation.
    hdr.srcHipBoneIndex = (ex.SourceHipBoneIndex == ~0u) ? kRetargetInvalidIndex : ex.SourceHipBoneIndex;
    hdr.tgtHipBoneIndex = (ex.TargetHipBoneIndex == ~0u) ? kRetargetInvalidIndex : ex.TargetHipBoneIndex;
    hdr.hipScale        = ex.HipScale;
    hdr.translationTgtBones     = ex.TranslationTargetBones.empty()
        ? 0u
        : PushU32Packed(m_RigData, ex.TranslationTargetBones.begin(), ex.TranslationTargetBones.end());
    hdr.translationTgtBoneCount = static_cast<uint32_t>(ex.TranslationTargetBones.size());

    // Skinning data sourced directly from SkeletonData (no recomputation).

    // Inverse-bind matrices: SkeletonData carries them precomputed per bone.
    // Falls back to identity ONLY in debug paths (synthetic skeletons used in
    // unit tests); production rigs always populate this.
    {
        const uint32_t invBindOff = static_cast<uint32_t>(m_RigData.size() / 4);
        const size_t expectedFloats = static_cast<size_t>(ex.TargetBoneCount) * 16;
        if (tgtSkel.InverseBind.size() >= expectedFloats)
        {
            for (uint32_t b = 0; b < ex.TargetBoneCount; ++b)
            {
                const float* p = tgtSkel.InverseBind.data() + b * 16;
                for (int i = 0; i < 16; i += 4)
                    PushVec4(m_RigData, p[i], p[i + 1], p[i + 2], p[i + 3]);
            }
        }
        else
        {
            Logger::Log::Warning("[RetargetGPUDataStore] tgtSkel.InverseBind missing for rig pair "
                                  "(expected {} floats, got {}). Producing identity skinning matrices — "
                                  "renderer output will be wrong.",
                                  expectedFloats, tgtSkel.InverseBind.size());
            for (uint32_t b = 0; b < ex.TargetBoneCount; ++b)
            {
                PushVec4(m_RigData, 1, 0, 0, 0);
                PushVec4(m_RigData, 0, 1, 0, 0);
                PushVec4(m_RigData, 0, 0, 1, 0);
                PushVec4(m_RigData, 0, 0, 0, 1);
            }
        }
        hdr.tgtInverseBind = invBindOff;
    }

    // Joint-node table: skin-joint -> tgt-skeleton-bone.
    //
    // FBX-imported skeletons sometimes leave JointNodes empty + SkinJointCount=0;
    // their CPU BuildPoseToSkinMatrices falls back to "joint j == bone j"
    // identity mapping. Mirror that here when caller passes skinJointCount
    // == BoneCount but JointNodes is empty.
    if (tgtSkel.JointNodes.empty() && skinJointCount == tgtSkel.BoneCount)
    {
        std::vector<uint32_t> identityMap(skinJointCount);
        for (uint32_t j = 0; j < skinJointCount; ++j) identityMap[j] = j;
        hdr.tgtJointNodes = PushU32Packed(m_RigData, identityMap.begin(), identityMap.end());
    }
    else if (tgtSkel.JointNodes.size() == skinJointCount)
    {
        hdr.tgtJointNodes = PushU32Packed(m_RigData, tgtSkel.JointNodes.begin(), tgtSkel.JointNodes.end());
    }
    else
    {
        Logger::Log::Error("[RetargetGPUDataStore] JointNodes size mismatch: tgtSkel has {} entries, "
                            "caller passed skinJointCount={} (BoneCount={}). GPU upload aborted.",
                            tgtSkel.JointNodes.size(), skinJointCount, tgtSkel.BoneCount);
        m_RigData.resize(headerVec4Off * 4);
        return kRetargetInvalidIndex;
    }

    // Mesh root inverse: invert SkeletonData's MeshRootWorld at upload time.
    // For mesh nodes at non-identity world transforms (common for grouped
    // imports), this is the difference between correct skinning and visible
    // offset bugs.
    {
        const uint32_t mriOff = static_cast<uint32_t>(m_RigData.size() / 4);
        glm::mat4 mrw{};
        std::memcpy(&mrw, tgtSkel.MeshRootWorld, sizeof(glm::mat4));
        const glm::mat4 mri = glm::inverse(mrw);
        const float* p = reinterpret_cast<const float*>(&mri);
        for (int i = 0; i < 16; i += 4)
            PushVec4(m_RigData, p[i], p[i + 1], p[i + 2], p[i + 3]);
        hdr.tgtMeshRootInverse = mriOff;
    }

    // Backfill the header into the reserved slot.
    const uint32_t hdrUintOff = headerVec4Off * 4;
    std::memcpy(m_RigData.data() + hdrUintOff, &hdr, sizeof(hdr));

    m_RigGuidToHeaderIdx[key] = headerVec4Off;
    m_RigDirty = true;
    UpdateCategoryBytes();
    return headerVec4Off;
}

uint32_t RetargetGPUDataStore::ResolveClipIdx(const GUID& guid) const
{
    const auto* clip = m_Clips.Find(PackGuid(guid));
    return clip ? clip->Slot : kRetargetInvalidIndex;
}

uint32_t RetargetGPUDataStore::ResolveRigPairIdx(const GUID& mapGuid) const
{
    auto it = m_RigGuidToHeaderIdx.find(PackGuid(mapGuid));
    return (it != m_RigGuidToHeaderIdx.end()) ? it->second : kRetargetInvalidIndex;
}

void RetargetGPUDataStore::ReleaseClip(const GUID& guid)
{
    m_Clips.Erase(PackGuid(guid));
    // A later upload rebuilds the live payloads. Keep logical slots fixed for
    // character parameters already assembled in this frame.
}

void RetargetGPUDataStore::ReleaseRigPair(const GUID& mapGuid)
{
    m_RigGuidToHeaderIdx.erase(PackGuid(mapGuid));
}

void RetargetGPUDataStore::AttachAssetEvents(AssetEventDispatcher& dispatcher)
{
    // Idempotent — replaces any previous subscription. The Reset() inside the
    // assignment-from-rvalue path of AssetReloadInvalidator handles cleanup of
    // the prior callback handle, so calling this twice (e.g. after a hot-
    // reload of AssetManager itself) doesn't accumulate dispatcher slots.
    //
    // AssetEventDispatcher fires synchronously on whoever called DispatchEvent
    // (typically the asset thread). The system thread mutates the same
    // GUID->headerIdx tables via EnsureClipUploaded / EnsureRigPairUploaded.
    // Direct mutation from the callback would race; instead, the callback only
    // queues the GUID and BeginFrame drains the queue from the single-threaded
    // frame-start site. Mirrors the deferred-queue pattern at
    // HumanoidRetargetSystem.cpp:323-337.
    m_ClipUnloadWatcher = AssetReloadInvalidator(
        dispatcher, AssetType::Animation,
        [this](const GUID& guid) {
            std::lock_guard<std::mutex> lk(m_PendingReleaseMutex);
            m_PendingClipReleases.push_back(guid);
        });

    m_MapUnloadWatcher = AssetReloadInvalidator(
        dispatcher, AssetType::RetargetMap,
        [this](const GUID& guid) {
            std::lock_guard<std::mutex> lk(m_PendingReleaseMutex);
            m_PendingMapReleases.push_back(guid);
        });
}

void RetargetGPUDataStore::AppendCharacterParams(std::span<const GPURetargetCharacterParams> params)
{
    m_Chars.insert(m_Chars.end(), params.begin(), params.end());
    m_ActiveCharCount = static_cast<uint32_t>(m_Chars.size());
    m_CharsDirty = true;
    UpdateCategoryBytes();
}

GameEngine::Rendering::BufferHandle RetargetGPUDataStore::CreateOrGrowBuffer(
    GameEngine::Rendering::BufferHandle existing,
    size_t& currentCapacity,
    size_t requiredSize,
    const char* debugName)
{
    if (!m_Device) return existing;
    if (requiredSize == 0) return existing;
    if (existing.IsValid() && currentCapacity >= requiredSize) return existing;

    // 1.5x geometric growth, vec4-aligned (16-byte). Dampens reallocation
    // thrash on incremental uploads while keeping memory waste bounded.
    size_t newCapacity = std::max(requiredSize, currentCapacity + currentCapacity / 2);
    newCapacity = (newCapacity + 15) & ~size_t{15};

    if (existing.IsValid()) m_Device->DestroyBuffer(existing);

    GameEngine::Rendering::BufferDesc desc{};
    desc.size = newCapacity;
    desc.usage = static_cast<uint32_t>(
        GameEngine::Rendering::BufferUsage::Storage |
        GameEngine::Rendering::BufferUsage::TransferDst);
    // Upload + PersistentlyMapped: host-visible memory the device reads
    // directly. UpdateBuffer becomes a simple memcpy into the persistent
    // mapping, no staging copy + transfer queue submission needed (which
    // would require explicit barriers before the compute dispatch reads it).
    // Mirrors GPUAnimationDataStore — same convention for the same reason.
    desc.memoryUsage = GameEngine::Rendering::BufferMemoryUsage::Upload;
    desc.flags = GameEngine::Rendering::BufferCreateFlags::PersistentlyMapped;
    desc.persistent = true;
    desc.debugName = debugName;

    auto handle = m_Device->CreateBuffer(desc);
    if (!handle.IsValid())
    {
        Logger::Log::Warning("[RetargetGPUDataStore] Failed to (re)create buffer '{}'.", debugName);
        currentCapacity = 0;
        return {};
    }
    currentCapacity = newCapacity;
    return handle;
}

void RetargetGPUDataStore::FlushIfDirty()
{
    if (!m_Device) return;

    if (m_ClipDirty && UploadClipData(m_ClipData))
        m_ClipDirty = false;

    if (m_RigDirty)
    {
        const size_t bytes = m_RigData.size() * sizeof(uint32_t);
        m_RigBuf = CreateOrGrowBuffer(m_RigBuf, m_RigBufCapacity, bytes, "RetargetRigBuf");
        if (m_RigBuf.IsValid() && bytes > 0)
            m_Device->UpdateBuffer(m_RigBuf, 0, bytes, m_RigData.data());
        m_RigDirty = false;
    }

    if (m_CharsDirty)
    {
        const size_t bytes = m_Chars.size() * sizeof(GPURetargetCharacterParams);
        m_CharsBuf = CreateOrGrowBuffer(m_CharsBuf, m_CharsBufCapacity, bytes, "RetargetCharsBuf");
        if (m_CharsBuf.IsValid() && bytes > 0)
            m_Device->UpdateBuffer(m_CharsBuf, 0, bytes, m_Chars.data());
        m_CharsDirty = false;
    }

    UpdateCategoryBytes();
}

void RetargetGPUDataStore::UpdateCategoryBytes()
{
    using ::GameEngine::Memory::AllocationCategory;
    using ::GameEngine::Memory::TrackAllocation;
    using ::GameEngine::Memory::TrackDeallocation;

    const uint64_t cpuBytes =
        m_ClipData.size() * sizeof(uint32_t)
      + m_RigData.size() * sizeof(uint32_t)
      + m_Chars.size() * sizeof(GPURetargetCharacterParams);
    const uint64_t devBytes =
        static_cast<uint64_t>(m_ClipBufCapacity) +
        static_cast<uint64_t>(m_RigBufCapacity) +
        static_cast<uint64_t>(m_CharsBufCapacity);
    const uint64_t newBytes = std::max(cpuBytes, devBytes);

    if (newBytes > m_ResidentBytes)
        TrackAllocation(AllocationCategory::Animation, newBytes - m_ResidentBytes);
    else if (newBytes < m_ResidentBytes)
        TrackDeallocation(AllocationCategory::Animation, m_ResidentBytes - newBytes);
    m_ResidentBytes = newBytes;
}

}}} // namespace GameEngine::Engine::Renderer
