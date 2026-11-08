#pragma once

#include "ECS/Systems.h"
#include "ECS/Entity.h"
#include "Types/Types.h"
#include "AssetCore/GUID.h"
#include "Assets/AnimationClip.h"
#include "Components/Animation/AnimatorRef.h"
#include "Engine/Rendering/AnimationSampling.h"
#include "Engine/Rendering/BoneLOD/BoneLODPolicy.h"

#include "AssetCore/AssetReloadInvalidator.h"
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine {
namespace ECS { template <typename T> class Writable; }
namespace Components { struct SkeletonRef; struct MeshRenderer; struct MorphTargetWeights; }
namespace Engine::Renderer {

class RenderServices;
class ClipStore;
class SkeletonStore;
// SkeletonData is provided by SkeletonStore.h via the Animation module shim.

// Collects animated entities per frame and dispatches animation work to the GPU
// compute skinning pipeline when the shader is loaded. Falls back to CPU
// sampling until the compute pass is ready (first few frames, or missing SPIR-V).
class AnimationSystem : public ECS::ISystem {
public:
    explicit AnimationSystem(RenderServices* rs) : m_RenderServices(rs) {}
    const char* GetName() const override { return "AnimationSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
    /// Last selection for diagnostics. Borrowed storage expires at the next Update.
    const BoneLODPolicy::Selection* GetBoneLODSelection(uint32 runtimeId) const
    {
        return m_BoneLODPolicy.Find(runtimeId);
    }

    /// Requires a selected tier within kernel limits and exact clip-channel index alignment.
    /// Cross-rig channels use CPU name resolution instead.
    bool IsClipSkeletonGPUSafe(uint32 skeletonId, uint32 clipIndex,
                               const SkeletonData& skeleton, ClipStore& clipStore,
                               const Animation::BoneEvaluationPlan* plan = nullptr);

private:
    void CollectWorkItem(ECS::World& world, ECS::EntityHandle entity,
        ECS::Writable<Components::AnimatorRef> animatorRow, const Components::SkeletonRef& reference,
        const Components::MeshRenderer* meshRenderer, Components::MorphTargetWeights* morphWeights,
        ClipStore& clipStore, SkeletonStore& skeletonStore, float32 deltaTime);

    RenderServices* m_RenderServices;
    BoneLODPolicy m_BoneLODPolicy;

    struct WorkItem {
        uint32 SkeletonId;
        uint32 RuntimeId;
        uint32 ClipIndex;
        float32 Time;
        const SkeletonData* CachedSkeleton;
        ECS::EntityHandle Entity; // for cross-rig bootstrap: AddComponent<HumanoidRetargeterComponent>
        uint32 PrevClipIndex = 0;
        float32 PrevTime = 0.0f;
        float32 BlendAlpha = 1.0f;
        uint64 RuntimeGeneration = 0u;
        const BoneLODPolicy::Selection* BoneSelection = nullptr;
        uint64 ClipRevision = 0u;
        uint64 PreviousClipRevision = 0u;
    };

    std::vector<WorkItem> m_WorkItems;
    std::vector<WorkItem> m_CompactedWorkItems;
    std::unordered_map<uint32, size_t> m_UniqueByRuntimeId;
    PoseSampleWorkspace m_Workspace; // CPU fallback (used until compute shader loads)
    PoseSampleWorkspace m_BlendFrom; // outgoing clip locals during a crossfade

    // Exact playback bits, model-plan identity, tier, and runtime generation
    // define palette content. A changed key invalidates skin-dependent depth.
    struct PaletteInputs
    {
        uint32 SkeletonId = 0;
        uint32 ClipIndex = 0;
        uint32 TimeBits = 0;
        uint32 PrevClipIndex = 0;
        uint32 PrevTimeBits = 0;
        uint32 BlendAlphaBits = 0;
        uint64 RuntimeGeneration = 0u;
        SharedPtr<const ModelBoneLOD> BoneData;
        bool BoneReduced = false;
        uint64 ClipRevision = 0u;
        uint64 PreviousClipRevision = 0u;
    };
    std::unordered_map<uint32, PaletteInputs> m_LastPaletteInputs;

    // Clip channel-index alignment, cleared when source assets change.
    // Kernel limits are checked for the current selection before this cache.
    struct ClipSafety
    {
        uint64 Revision;
        bool Safe;
    };
    std::unordered_map<uint64_t, ClipSafety> m_GPUSafeCache;

    // Cross-rig humanoid retargeting lives in HumanoidRetargetSystem (Phase
    // 6). It runs immediately after this system in the Animation wave and
    // owns the source-clip sample, RetargetNode dispatch, and GPU prime for
    // entities carrying a HumanoidRetargeterComponent. Same-rig sampling
    // stays here unchanged.

    // Same-rig decisions and failed bootstrap attempts. Successful components
    // stay entity-owned so Play restore can bootstrap them again.
    std::shared_ptr<std::atomic<bool>> m_BootstrapContentDirty = std::make_shared<std::atomic<bool>>(false);
    std::array<AssetReloadInvalidator, 5> m_BootstrapWatchers;
    uint64 m_BootstrapSourceVersion = 0;
    uint64 m_BootstrapWorldId = 0;
    uint64 m_BootstrapWorldReset = 0;
    bool m_BootstrapWatching = false;
    std::unordered_set<uint64_t> m_BootstrapAttempted;
};

// Test hooks: enable unit tests to inject a parsed clip without filesystem/cgltf
namespace TestHooks {
    bool SetClipCacheForTest(const GUID& guid, SharedPtr<AnimationClip> clip);
    void ClearClipCacheForTest();
    // CPU sampling path for tests without ECS or GPU device.
    void SampleByRefsForTest(Components::AnimatorRef& animRef, uint32 skeletonId, float32 deltaTime);
    // Runtime ID most recently created/used by SampleByRefsForTest. Lets tests
    // inspect the sampled pose via SkeletonStore::GetRuntime(). Returns 0 if
    // SampleByRefsForTest has not yet run.
    uint32 GetLastTestRuntimeId();
    // Probe AnimationSystem::IsClipSkeletonGPUSafe without touching the ECS
    // dispatcher — the gate's per-(skel,clip) decision is what tests need to
    // verify. Constructs an internal AnimationSystem on demand; returns true
    // when the GPU compute path can safely consume the pair (the skeleton fits
    // the kernel's limits, and every channel's stored boneIndex points to a
    // bone whose name hashes to its targetNameId, OR the channel is a legacy
    // targetNameId==0 channel).
    bool IsClipSkeletonGPUSafeForTest(uint32 skeletonId, uint32 clipIndex);
}

} } // namespace GameEngine::Engine::Renderer
