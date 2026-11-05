// MaterialBinder is the single name-driven binding service for the engine. It
// walks the shader's reflected ShaderMeta and for each declared descriptor
// set resolves every binding by StringId against two resource sources, in
// priority order:
//
//   1. DrawBindings  (per-draw, most specific)
//   2. PassResources (per-pass, world-shared — Cam, Light, MaterialParams SSBO,
//      shadow map, MSM moments, etc. all keyed by the reflected name the
//      shader declared)
//
// Material params + textures are NOT a per-set source: they live in the shared
// MaterialParams SSBO (bound as a PassResource) and the global bindless texture
// set, both indexed per-instance. The per-material UBO/texture fallback was
// removed in D1 — every device runs the bindless SSBO path.
//
// The binder has no per-set-index policy — "set 0 is camera", "set 1 is
// material", "set 2+ is per-draw" is no longer hardcoded. Each set's lifetime
// (and therefore its cache scope) is classified by which source its bindings
// drew from:
//
//   - Any binding from DrawBindings  -> per-draw  (no cache, transient)
//   - Else reuse the exact resolved resources within the pass scope
//
// Bindless short-circuit: a set whose layout exactly matches the engine's
// global bindless texture set (the `ge_BindlessTextures` array) returns the
// global descriptor set directly — no per-set allocation.
//
// Caches clear on frame boundaries (OnBeginFrame). Threading: recording
// workers share immutable entries under a reader/writer lock.

#pragma once

#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/PassBindingContext.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/HashUtils.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/StringId.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <memory>
#include <vector>

namespace GameEngine::Rendering
{
class CommandList;
class IDevice;
struct ShaderMeta;
struct DescriptorSetMeta;
struct DescriptorBindingPlan;
struct DescriptorBindingMeta;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

class Material;
class RenderServices;

/// Device handles resolved for one draw in this device frame.
struct PreparedDrawBindings
{
    Rendering::PipelineHandle Pipeline{};
    std::array<Rendering::DescriptorSetHandle, kMaxDescriptorSets> Sets{};
    std::array<Rendering::DescriptorSetLayoutId, kMaxDescriptorSets> Layouts{};
    uint32_t LayoutCount = 0;
};

class MaterialBinder
{
  public:
    MaterialBinder(RenderServices& services, ::GameEngine::Rendering::IDevice& device);
    ~MaterialBinder();

    MaterialBinder(const MaterialBinder&) = delete;
    MaterialBinder& operator=(const MaterialBinder&) = delete;

    // Begins a pass-scope binding session. The per-pass resource table arrives
    // PRE-RESOLVED — RenderGraph callers build it at declaration from AllocUpload/import
    // handles, so there is no logical-handle resolution to run against a
    // context. Returns a stateful context the caller threads through every
    // BindMaterialForDraw call inside the pass.
    //
    // `passInstanceIndex` disambiguates multiple instances of the same logical
    // pass that share (view, keywords, layout) but bind distinct per-pass
    // resources — shadow cascades pass their cascade index; depth prepass and
    // forward pass 0. Routes through the per-pass descriptor-set cache key so
    // each instance gets its own cached set.
    PassBindingContext BeginPass(
        ::GameEngine::Rendering::CommandList& cmd,
        ::GameEngine::Rendering::ViewId view,
        const ::GameEngine::Rendering::RenderGraph::RGContext& passCtx,
        ::GameEngine::Rendering::MaterialKeyword passKeywords,
        ResolvedPassResources resources,
        uint32_t passInstanceIndex = 0);

    /// Resolve the sets of the served material variant without recording commands.
    PreparedDrawBindings PrepareMaterialForDraw(
        const PassBindingContext& pass, const Material& material, Rendering::PipelineHandle pipeline,
        const Rendering::ShaderMeta* servedMeta, const DrawBindings& draw, uint32_t pipelineSetCount = 0,
        std::span<const Rendering::DescriptorSetLayoutId> pipelineSetLayouts = {});

    /// Resolve a materialless pipeline's sets from draw and pass resources.
    PreparedDrawBindings PreparePipelineForDraw(
        const PassBindingContext& pass, Rendering::PipelineHandle pipeline, const Rendering::ShaderMeta& meta,
        const DrawBindings& draw, uint32_t pipelineSetCount = 0,
        std::span<const Rendering::DescriptorSetLayoutId> pipelineSetLayouts = {});

    /// Bind a prepared packet against the sticky state of this command encoder.
    Rendering::PipelineHandle BindPreparedForDraw(
        PassBindingContext& pass, const PreparedDrawBindings& prepared, std::span<const std::byte> pushConstants = {});

    // Binds pipeline + every reflected descriptor set + push constants. The
    // binder walks `material.GetShaderMeta()->Sets` and resolves each set
    // independently by name — no set-index policy. Returns the resolved
    // pipeline handle so callers can chain SetVertexBuffer / SetIndexBuffer
    // / DrawIndexed using it.
    //
    // `preResolvedPipeline`: when valid, the binder skips its own pipeline
    // resolution and uses this handle directly. Callers that maintain their
    // own variant cache (e.g. the world pass's instanced-format-keyed cache,
    // the depth pass's narrowed pipelines) resolve there and hand the result
    // in. When invalid (default), the binder resolves via
    // material.GetGraphicsPipelineIdForFlags + passCtx.GetOrCreatePipelineVariant.
    //
    // `pipelineSetCount`: when non-zero, bounds the set walk to indices
    // [0, pipelineSetCount). Depth-only pipelines compile only the vertex
    // stage and declare a single set 0, while the material's reflected meta
    // still carries fragment-stage sets — binding them would violate
    // VUID-vkCmdSetDescriptorBufferOffsetsEXT-firstSet-08066. Callers using
    // narrowed pipelines must pass the pipeline's actual set-layout count.
    //
    // `metaOverride`: when non-null, the binder walks this meta instead of
    // `material.GetShaderMeta()`. Required when the pipeline was compiled
    // from a different keyword set than the material's base — e.g. the
    // world-pass instanced variant adds Instanced/ForwardPlus/Shadows which
    // introduce extra set-0 bindings (Light/Cluster/Shadow SSBOs + shadow
    // textures) that the base material's reflection doesn't see.
    //
    // `pipelineSetLayouts`: when non-empty, the binder uses these layout
    // ids on a pipeline change to find the first set whose layout differs
    // from the prior pipeline's and only invalidate the sticky-bind cache
    // from that set onward (Vulkan's pipeline-layout-compatibility rule).
    // When empty, the binder conservatively clears all sticky-bind
    // tracking — same behavior as before this optimisation. Passing
    // precomputed ids avoids the device's mutex-guarded
    // LookupGraphicsPipeline on every SetPipeline; cache them once at
    // variant-compile time and reuse.
    ::GameEngine::Rendering::PipelineHandle BindMaterialForDraw(
        PassBindingContext& pass,
        const Material& material,
        ::GameEngine::Rendering::VertexAttributeFlags meshVertexFlags,
        const DrawBindings& draw,
        ::GameEngine::Rendering::PipelineHandle preResolvedPipeline = {},
        const ::GameEngine::Rendering::ShaderMeta* metaOverride = nullptr,
        uint32_t pipelineSetCount = 0,
        std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts = {});

    // Bind a raw graphics/compute pipeline that has no backing Material
    // (terrain depth, debug-line draws, particle emitters, decals). Walks
    // `meta.Sets` and resolves each binding by `StringId` against two sources:
    //
    //   1. DrawBindings  (per-draw)
    //   2. PassResources (per-pass — Cam, Light, shadow textures, etc.)
    //
    // Bindless texture sets short-circuit to the engine's global handle. There
    // is no per-material source — callers carrying a per-pipeline UBO push it
    // through DrawBindings.
    //
    // `pipelineSetCount` mirrors the BindMaterialForDraw parameter: when
    // non-zero it bounds the set walk to `[0, pipelineSetCount)` so callers
    // can use narrowed pipeline layouts whose set count is smaller than the
    // reflected meta's set count.
    ::GameEngine::Rendering::PipelineHandle BindPipelineForDraw(
        PassBindingContext& pass,
        ::GameEngine::Rendering::PipelineHandle pipeline,
        const ::GameEngine::Rendering::ShaderMeta& meta,
        const DrawBindings& draw,
        uint32_t pipelineSetCount = 0,
        std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts = {});

    // Symmetric pass terminator. No-op today; reserved for parallel-record
    // finalization hooks.
    void EndPass(PassBindingContext& pass);

    // Apply Vulkan's pipeline-layout-compatibility rule on a pipeline change:
    // bindings stay sticky up to and including the highest set whose layout
    // is identical between the old and new pipelines; sets from the first
    // differing layout upward are invalidated. When `pipelineSetLayouts` is
    // empty (caller didn't precompute them), the binder conservatively
    // clears every set (legacy behavior). Public so callers that bypass
    // BindMaterialForDraw / BindPipelineForDraw can maintain consistent
    // state.
    void InvalidateStickyBindsForPipeline(
        PassBindingContext& pass,
        std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts);

    // Frame-boundary cache invalidation. Wired into
    // RenderServices::BeginWorldDrawFrame so transient descriptor sets don't
    // outlive the per-frame DB pool reset.
    void OnBeginFrame();

    // Build (or fetch from cache) the descriptor set for the given set index
    // declared by the material's reflected meta. Returns an invalid handle if
    // the set isn't in the meta or has no bindings. Public so siblings like
    // BindPipelineForDraw can reuse the same construction path.
    ::GameEngine::Rendering::DescriptorSetHandle BuildSetForBinding(
        PassBindingContext& pass,
        const Material& material,
        uint32_t setIdx,
        const DrawBindings& draw);

    // Variant of BuildSetForBinding that accepts a pre-located setMeta. Used
    // when the caller already walked an override meta (e.g. the world pass's
    // compiled variant reflection, which differs from the material's stored
    // base meta). The setMeta pointer must outlive the call.
    // `outUnresolvable`, when non-null, is set true if a statically-used
    // uniform/storage buffer binding in this set resolved from neither
    // DrawBindings nor PassResources, or an unresolved sampled-image binding's
    // shape has no slot default (ResolveSlotDefault). BindMaterialForDraw uses it to SKIP the
    // draw rather than issue it with an unbound (garbage-address) descriptor.
    ::GameEngine::Rendering::DescriptorSetHandle BuildSetForBindingFromMeta(
        const PassBindingContext& pass,
        const Material& material,
        const ::GameEngine::Rendering::DescriptorSetMeta& setMeta,
        const DrawBindings& draw,
        bool* outUnresolvable = nullptr);

    // What a sampled-image slot that no source provides is bound to. The default has the
    // slot's own shape: a depth slot gets a reverse-Z-far depth texture of its view dimension
    // with the comparison sampler (a shadow tap against it reads fully lit), a cube slot the
    // black cube, a 2D-array slot the white array, a 2D slot the per-name 1x1 default.
    // Sampler is the one the binder writes beside it: the comparison sampler for a depth slot,
    // `materialSampler` otherwise. This is the binder's whole decision for the slot, which is
    // why it is public: MaterialBinderTests pins it per shape. Texture is invalid
    // for a shape with no default (3D, multisampled, integer, a depth or arrayed cube): the
    // binder then flags the set unresolvable, so the draw is skipped with an error naming the
    // slot, because a default of the wrong shape or an unwritten entry is an invalid bind group
    // on WebGPU and an undefined read on any backend that uses the slot.
    struct SlotDefault
    {
        ::GameEngine::Rendering::TextureHandle Texture;
        ::GameEngine::Rendering::SamplerHandle Sampler;
    };
    SlotDefault ResolveSlotDefault(const ::GameEngine::Rendering::DescriptorBindingMeta& binding,
                                   ::GameEngine::Rendering::SamplerHandle materialSampler);

  private:
    // One entry per plan slot, in reflection order. Flat rows let a worker
    // resolve and compare a set without allocating per-binding write arrays.
    struct ResolvedBinding
    {
        Rendering::BufferHandle Buffer{};
        Rendering::TextureHandle Texture{};
        Rendering::SamplerHandle Sampler{};
        uint64_t Offset = 0;
        uint64_t Range = 0;
        uint32_t ArrayIndex = 0;
        bool operator==(const ResolvedBinding&) const = default;
    };

    struct Resolution
    {
        bool PerDraw = false;
        bool Unresolvable = false;
    };
    Resolution ResolveBindings(const ResolvedPassResources& pass, const Material& material, uint32_t setIndex,
                               const Rendering::DescriptorBindingPlan& plan, const DrawBindings& draw,
                               std::vector<ResolvedBinding>& writes);
    Rendering::DescriptorSetHandle BuildSetInternal(const Rendering::DescriptorSetMeta& setMeta,
                                                    const Rendering::DescriptorBindingPlan& plan,
                                                    std::span<const ResolvedBinding> writes);

    // Build a transient descriptor set for the materialless path. Resolves
    // bindings against DrawBindings and PassResources only — no material
    // sources. Used by BindPipelineForDraw.
    ::GameEngine::Rendering::DescriptorSetHandle BuildPipelineSet(
        const PassBindingContext& pass,
        const ::GameEngine::Rendering::DescriptorSetMeta& setMeta,
        const Rendering::DescriptorBindingPlan& plan, const DrawBindings& draw);

    // A digest selects a bucket. Exact plan and write equality below decide
    // whether the cached descriptor may be reused.
    struct PerPassCacheKey
    {
        uint64_t ViewBits;
        uint64_t PassKwBits;
        uint64_t ContentsHash;
        uint32_t SetIndex;
        uint32_t PassInstanceIndex;
        bool operator==(const PerPassCacheKey&) const = default;
    };
    struct PerPassCacheKeyHash
    {
        size_t operator()(const PerPassCacheKey& k) const noexcept
        {
            using Rendering::HashUtils::HashCombine;
            uint64_t h = k.ViewBits;
            h = HashCombine(h, k.PassKwBits);
            h = HashCombine(h, k.ContentsHash);
            h = HashCombine(h, k.SetIndex);
            h = HashCombine(h, k.PassInstanceIndex);
            return static_cast<size_t>(h);
        }
    };

    // Warn-once key for missing bindings. Per (material, setIdx) so unrelated
    // materials with similar layouts don't suppress each other's warnings.
    // The warn maps are per-worker thread_local (A2.4-D6) — see the .cpp — so a
    // genuinely-missing binding may log once per worker that hits it (harmless).
    // WarnKey/WarnKeyHash key the thread_local map in ResolveBindings.
    struct WarnKey
    {
        const Material* Mat;
        uint32_t        SetIdx;
        bool operator==(const WarnKey& o) const noexcept
        {
            return Mat == o.Mat && SetIdx == o.SetIdx;
        }
    };
    struct WarnKeyHash
    {
        size_t operator()(const WarnKey& k) const noexcept
        {
            const uint64_t h =
                Rendering::HashUtils::HashCombine(std::hash<const void*>{}(k.Mat), k.SetIdx);
            return static_cast<size_t>(h);
        }
    };

    // Debug-only ownership check: a cold per-pass insert must hold the exclusive
    // lock. No-op in release. See PipelineVariantCache::AssertInsertLockHeld.
    void AssertPerPassLockHeld() const noexcept;

    RenderServices&                 m_Services;
    ::GameEngine::Rendering::IDevice& m_Device;

    // Workers share immutable entries. Insertion holds the exclusive lock.
    // OnBeginFrame clears the map before any recording workers start.
    struct CachedSet
    {
        std::shared_ptr<const Rendering::DescriptorBindingPlan> Plan;
        std::vector<ResolvedBinding> Writes;
        Rendering::DescriptorSetHandle Handle;
        bool Matches(const Rendering::DescriptorBindingPlan& plan,
                     const std::vector<ResolvedBinding>& writes) const;
    };
    std::unordered_multimap<PerPassCacheKey, CachedSet, PerPassCacheKeyHash> m_PerPassCache;
    mutable std::shared_mutex m_PerPassCacheMutex;

    // Hands out the epoch values that stamp L1 slots. Strictly increasing and
    // never reused for the process lifetime, so an epoch identifies both the
    // binder that filled a slot and the frame it filled it in. Neither an
    // address nor a per-binder counter can do that: the allocator reissues a
    // dead binder's address, and every binder would start its counter at the
    // same value.
    static uint64_t NextPerPassEpoch() noexcept;

    // Redrawn by OnBeginFrame's clear, which retires every L1 slot holding a
    // handle from the frame just ended (the transient pool resets underneath
    // them), and drawn once at construction so no slot any earlier binder left
    // behind can validate against this one.
    std::atomic<uint64_t> m_PerPassEpoch;
#if GE_DEBUG_INSTRUMENTATION
    bool m_PerPassInsertLockHeldDebug = false;
#endif

    // Per-thread L1 points into stable nodes of m_PerPassCache until frame reset.
    // Slots outlive the binder and are only valid while their epoch is current;
    // zero is never issued as an epoch, so a zeroed slot never validates.
    static constexpr size_t kPerPassL1Slots = 256;
    struct PerPassL1Slot
    {
        PerPassCacheKey Key{};
        const CachedSet* Value = nullptr;
        uint64_t Epoch = 0;
    };
};

} // namespace GameEngine::Engine::Renderer
