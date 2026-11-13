// WorldDrawBuilder: per-view batch-key derivation for the GPU-driven draw path.

#pragma once

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Types/FalseSharing.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
namespace Engine::Renderer
{
class Material;

// Engine-level helper that consumes WorldSubmissionRecord batches from
// extraction systems and derives per-view sets of unique (Material*, mesh,
// materialIndex, meshIndex) batch keys for the GPU-driven indirect-draw
// path. The bucketer dispatch scheduler reads the keys to schedule one
// compute pass per (view, batch); the world color / depth-pass execute
// lambdas iterate the same keys and issue one DrawIndexedIndirectCount per
// batch, consuming the bucketer-written indirection slot for the per-batch
// firstInstance->GPUScene-index mapping.
//
// Ownership and lifetime are managed by RenderServices; this type is
// exposed so systems can reason about behavior in tests and advanced
// integrations.
class WorldDrawBuilder
{
  public:
    // `jobSystem` is the pool whose workers submit: one submission buffer per
    // worker plus one for callers off its workers (the render thread). Null
    // when every submit comes from a single thread (hosts without a pool).
    explicit WorldDrawBuilder(const JobSystem::WorkStealingThreadPool* jobSystem);

    // The setRevision of a submit that names no record set.
    static constexpr uint64_t kNoRecordSet = 0;

    // Called once per frame on the render thread before any submissions.
    void BeginFrame();

    // Submit a batch of records. Each worker of the construction pool appends to
    // its own buffer. Off the pool, only the constructing thread may submit. Call
    // MergeSubmissions() on the render thread after all submissions are done.
    // `setRevision` names an unchanged record set (KeyedWorldSubmissions::Revision);
    // kNoRecordSet names none, and the next BuildBatchKeys derives every view again.
    void Submit(std::span<const WorldSubmissionRecord> records, uint64_t setRevision = kNoRecordSet);

    // Merge per-thread submission buffers into the per-view map, in worker-id
    // order with the off-pool buffer last. Must be called on the render thread
    // after all parallel submissions are done and before BuildBatchKeys.
    void MergeSubmissions();

    // Derive per-view batch-key sets from the merged submissions. Calls
    // MergeSubmissions() internally. The mesh registry is required so
    // submissions whose mesh entry isn't resident yet (async upload race)
    // are filtered out before the unique sort. `materialColorClass` (indexed
    // by materialIndex) is the P2 color-class map: when non-empty, keys are
    // grouped/deduped on (colorClassId, meshIndex) so same-PSO opaque
    // materials share one batch; an empty span keeps the (materialIndex,
    // meshIndex) key identity (colorClassId == materialIndex), today's path.
    // Returns the number of views whose keys were (re)derived this call — the
    // A2 STEP-0 sort-bracket view count.
    //
    // Keeps the last build and returns 0 while the named sets, the mesh content revision, the generation
    // (it guards the material pointers) and the key inputs of each material read stay the same. Call order
    // in a frame: BeginFrame, the submits, one BuildBatchKeys; a repeat with no new submit keeps the views.
    uint32_t BuildBatchKeys(const Rendering::MeshGPURegistry& meshRegistry,
                            std::span<const uint32_t> materialColorClass,
                            uint64_t materialIndexGeneration); // MaterialSystem::MaterialSSBOGeneration

    // Build batch keys for a single view without touching other views. Used
    // by editor-driven one-shot views (e.g. thumbnail renders) so a late
    // thumbnail submission doesn't clobber Scene/Game view state mid-frame.
    void BuildBatchKeysForView(Rendering::ViewId viewId,
                               const Rendering::MeshGPURegistry& meshRegistry,
                               std::span<const uint32_t> materialColorClass = {});

    // Unique (material, mesh, materialIndex, meshIndex) keys per view. The
    // bucketer dispatch scheduler and the world/depth-pass execute lambdas
    // both consume this set — one bucketer dispatch + one indirect draw per
    // entry. The (materialIndex, meshIndex) pair is the streamKey identity;
    // the (Material*, MeshGPUHandle) pair gives the execute lambdas the
    // resources they need to bind without an extra registry lookup.
    struct BatchKey
    {
        const Material* material;
        Rendering::MeshGPUHandle mesh;
        uint32_t materialIndex;
        uint32_t meshIndex;
        // P2 color-class id (== materialIndex when the merge is off). The color
        // pass + main-view depth prepass look up their draw range by this;
        // shadow cascades substitute a shared-depth sentinel for eligible
        // casters and keep this id for the rest. ResolveDrawStreamLookupKey
        // owns that derivation — never key a range lookup off these fields
        // directly.
        uint32_t colorClassId;
    };
    std::span<const BatchKey> GetBatchKeys(Rendering::ViewId viewId) const;
    struct ShadowSubmissionState
    {
        uint64_t MembershipKey = 0;
        uint64_t MembershipCount = 0;
        uint64_t MaterialContentKey = 0;
        bool Complete = false;
    };
    struct ShadowSubmissionStates
    {
        ShadowSubmissionState All;
        ShadowSubmissionState Rigid;
    };
    /** Read this frame's caster census after BuildBatchKeys, on the render thread.
     *  Completeness covers submission filters; depth preparation must refine it. */
    ShadowSubmissionStates GetShadowSubmissionStates(Rendering::ViewId viewId) const;
    // The subset of GetBatchKeys whose material is in the deforming-motion
    // lane (MaterialDeformationClassify.h). Derived in the same pass as the
    // keys, so it costs one predicate per surviving key and carries identical
    // staleness; empty for a view with no deforming material, which is what
    // makes a frame with none pay one branch and no work. The deforming-motion
    // producer records from this span and the mover lane excludes exactly its members.
    std::span<const BatchKey> GetDeformingBatchKeys(Rendering::ViewId viewId) const;

    // True when this view has at least one submission with the cast-shadows
    // bit, drawable or not. Derived alongside the batch keys, so the shadow
    // gates that ask it several times per view per frame read one flag.
    bool HasShadowCastingSubmissions(Rendering::ViewId viewId) const;

    // True when this view has at least one drawable submission that is BOTH
    // transmissive and a shadow caster — the only way the glass-tint shadow
    // cascade can receive content, because the shadow cull drops non-casters
    // (InstanceInPass in frustum_culling.comp and hzb_culling.comp) before the tint pass's
    // indirect draw ever reads a stream. Derived alongside the batch keys, so it
    // costs one branch in a loop the frame already walks; a submissions scan of
    // its own would cost more than the cascade it gates.
    bool HasTransmissiveCasterSubmissions(Rendering::ViewId viewId) const;

    // True when this view has at least one submission whose material moves its
    // own vertices, and the shadow-casting subset of the same question. Such a
    // surface is displaced in the vertex stage from the shared animation clock,
    // so its rasterized depth differs from last frame's while every instance
    // record stays byte-identical — the depth-derived caches have no other
    // signal for it. Derived alongside the batch keys, the one place every
    // producer's records meet, so it costs one branch in a loop the frame
    // already walks.
    bool HasAnimatedVertexModifierSubmissions(Rendering::ViewId viewId) const;
    bool HasAnimatedVertexModifierCasterSubmissions(Rendering::ViewId viewId) const;

    // Per-instance merged submissions for a view (pre-dedup, unlike GetBatchKeys).
    // The sorted transparent path (T2) needs per-instance identity — the GPUScene
    // instanceIndex on each record — which the deduped batch keys discard, so it
    // reads this list, filters to Blend materials, and sorts by view distance.
    // Populated by MergeSubmissions (invoked from BuildBatchKeys) before pipeline
    // declaration. Empty span for a view with no submissions.
    std::span<const WorldSubmissionRecord> GetSubmissions(Rendering::ViewId viewId) const;

    // Clear submissions + batch keys for a single view. This is useful for
    // editor-driven one-shot views (e.g. thumbnail renders) so the view
    // doesn't keep re-rendering stale state across frames.
    void ClearView(Rendering::ViewId viewId);

    // Optional: clear all internal state without starting a new frame.
    void Clear();

  private:
    // One view's merged submissions and everything derived from them in one pass.
    struct ViewBatchState
    {
        std::vector<WorldSubmissionRecord> Submissions;
        std::vector<BatchKey> Keys;
        // The deforming subset of Keys.
        std::vector<BatchKey> DeformingKeys;
        ShadowSubmissionStates ShadowSubmissions;
        bool ShadowCaster = false;
        bool TransmissiveCaster = false;
        bool AnimatedVertexModifier = false;
        bool AnimatedVertexModifierCaster = false;

        // Empties the state and keeps the vector capacity.
        void Reset();
    };

    // The inputs of the last full BuildBatchKeys. The build is kept while they compare equal.
    struct BuildInputs
    {
        std::vector<uint64_t> SubmissionRevisions; // sorted
        uint64_t MeshContentRevision = 0;
        uint64_t MaterialIndexGeneration = 0;
        // False when a Submit named no set, a merge ran after the build, a view was cleared,
        // or a frame ran no build.
        bool Certified = false;

        bool SameInputs(const BuildInputs& other) const;
    };

    // The key inputs one material gave the last full build.
    struct MaterialKeyState
    {
        const Material* Source = nullptr;
        uint32_t MaterialIndex = 0;
        uint32_t ColorClassId = 0;
        uint32_t Predicates = 0; // bits of MaterialKeyPredicates (WorldDrawBuilder.cpp)
        uint64_t ContentRevision = 0;
        uint64_t ProgramVersion = 0;
    };

    // Derive the unique (matIdx, mshIdx) batch-key set for a view from its
    // merged submissions. Shared by BuildBatchKeys + BuildBatchKeysForView.
    // A non-null `materialsRead` collects every material the derivation reads.
    void DeriveBatchKeysForView(ViewBatchState& view,
                                const Rendering::MeshGPURegistry& meshRegistry,
                                std::span<const uint32_t> materialColorClass,
                                std::vector<const Material*>* materialsRead);

    // Fills m_FrameInputs, reusing its capacity.
    void CaptureInputs(const Rendering::MeshGPURegistry& meshRegistry, uint64_t materialIndexGeneration);
    bool MaterialsGiveSameKeyInputs(std::span<const uint32_t> materialColorClass) const;
    void CaptureMaterialKeyStates(std::span<const uint32_t> materialColorClass);
    // Marks every submitted record as merged without merging it.
    void DropUnmergedSubmissions();

    // --- Per-thread submission buffers (thread-safe Submit) ---

    struct ThreadSubmissionBuffer
    {
        std::vector<WorldSubmissionRecord> records;
        // The record set each Submit of this frame named (kNoRecordSet: none).
        std::vector<uint64_t> setRevisions;
        uint32 drained = 0; // records.size() at the time of last MergeSubmissions drain
    };
    // Null when the builder serves no pool; every submit then lands in the
    // off-pool buffer.
    const JobSystem::WorkStealingThreadPool* m_JobSystem;
    // The pool's worker count, which is also the index of the off-pool buffer.
    size_t m_OffPoolBufferIndex;
    // Indexed by worker id; the last entry takes every caller off the pool.
    // Sized once at construction and never resized, so a buffer's address is
    // stable while workers append to it.
    std::vector<FalseSharingPadded<ThreadSubmissionBuffer>> m_ThreadBuffers;
    // The one thread off the pool that may submit. Debug builds assert it.
    std::thread::id m_OwnerThreadId;

    // This frame's per-view state, populated by MergeSubmissions and the key derivation.
    FlatMap<Rendering::ViewId, ViewBatchState> m_Views;
    // The previous frame's per-view state. BeginFrame swaps it with m_Views, so a kept
    // build swaps it back instead of copying, and a full build reuses its capacity.
    FlatMap<Rendering::ViewId, ViewBatchState> m_PreviousViews;
    BuildInputs m_LastBuildInputs;
    BuildInputs m_FrameInputs;
    std::vector<MaterialKeyState> m_LastBuildMaterials;
    // Scratch of the full build: the materials it read, and a mark per material index.
    std::vector<const Material*> m_MaterialsRead;
    std::vector<const Material*> m_MaterialReadByIndex;
    // True when the full build read a material that has no material index yet.
    bool m_ReadUnindexedMaterial = false;
    // True once a merge took a record this frame.
    bool m_MergedThisFrame = false;
    // True once BuildBatchKeys ran this frame.
    bool m_BuiltThisFrame = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
