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
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/HashUtils.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Types/StringId.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
class CommandList;
class IDevice;
struct ShaderMeta;
struct DescriptorSetMeta;
struct DescriptorBindingPlan;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

class Material;
class RenderServices;

/// Immutable binding handles and copied layouts for one device frame.
/// The owning pass retains the GPU resources until its commands complete.
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

    /// Start a binding session on the thread that owns the command list.
    /// Resources are resolved device handles; each pass instance has an independent cache scope.
    /// The returned sticky state belongs to this encoder and device frame.
    PassBindingContext BeginPass(
        ::GameEngine::Rendering::CommandList& cmd,
        ::GameEngine::Rendering::ViewId view,
        const ::GameEngine::Rendering::RenderGraph::RGContext& passCtx,
        ::GameEngine::Rendering::MaterialKeyword passKeywords,
        ResolvedPassResources resources,
        uint32_t passInstanceIndex = 0);

    /// Resolve the served variant's sets during graph preparation, without commands.
    /// Inputs need only outlive this call; the returned handles expire at device-frame reset.
    /// A nonzero set count bounds the reflected set walk; layouts must match the pipeline.
    PreparedDrawBindings PrepareMaterialForDraw(
        const PassBindingContext& pass, const Material& material, Rendering::PipelineHandle pipeline,
        const Rendering::ShaderMeta* servedMeta, const DrawBindings& draw, uint32_t pipelineSetCount = 0,
        std::span<const Rendering::DescriptorSetLayoutId> pipelineSetLayouts = {});

    /// Resolve a materialless pipeline's sets during graph preparation, without commands.
    /// Inputs need only outlive this call; retain their GPU resources through command completion.
    /// A nonzero set count bounds the reflected set walk; layouts must match the pipeline.
    PreparedDrawBindings PreparePipelineForDraw(
        const PassBindingContext& pass, Rendering::PipelineHandle pipeline, const Rendering::ShaderMeta& meta,
        const DrawBindings& draw, uint32_t pipelineSetCount = 0,
        std::span<const Rendering::DescriptorSetLayoutId> pipelineSetLayouts = {});

    /// Record bindings on the thread that owns pass.Cmd and its sticky state.
    /// The packet must belong to this device frame; push-constant bytes need only outlive the call.
    Rendering::PipelineHandle BindPreparedForDraw(
        PassBindingContext& pass, const PreparedDrawBindings& prepared, std::span<const std::byte> pushConstants = {});

    // Symmetric pass terminator. No-op today; reserved for parallel-record
    // finalization hooks.
    void EndPass(PassBindingContext& pass);

    /// Invalidate sticky sets from the first incompatible pipeline layout upward.
    /// Empty layout IDs invalidate all sets; call on the thread that owns this encoder.
    void InvalidateStickyBindsForPipeline(
        PassBindingContext& pass,
        std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts);

    // Frame-boundary cache invalidation. Wired into
    // RenderServices::BeginWorldDrawFrame so transient descriptor sets don't
    // outlive the per-frame DB pool reset.
    void OnBeginFrame();

    // Build (or fetch from cache) the descriptor set for the given set index
    // declared by the material's reflected meta. Returns an invalid handle if
    // the set is not in the meta or has no bindings.
    ::GameEngine::Rendering::DescriptorSetHandle BuildSetForBinding(
        PassBindingContext& pass,
        const Material& material,
        uint32_t setIdx,
        const DrawBindings& draw);

    /// Resolve one reflected set during preparation; inputs need only outlive this call.
    /// Set outUnresolvable when a required buffer or image has no provider or valid fallback.
    /// Returned transient handles expire at device-frame reset; retain GPU resources through execution.
    ::GameEngine::Rendering::DescriptorSetHandle BuildSetForBindingFromMeta(
        const PassBindingContext& pass,
        const Material& material,
        const ::GameEngine::Rendering::DescriptorSetMeta& setMeta,
        const DrawBindings& draw,
        bool* outUnresolvable = nullptr);

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
    Rendering::SamplerHandle ResolveMaterialSampler(const Material& material,
                                                    Rendering::SamplerHandle& cachedSampler);
    static bool ShouldWarnBinding(const Material& material, uint32_t setIndex, uint32_t binding);
    Rendering::DescriptorSetHandle BuildSetInternal(const Rendering::DescriptorSetMeta& setMeta,
                                                    const Rendering::DescriptorBindingPlan& plan,
                                                    std::span<const ResolvedBinding> writes);

    // Build a transient descriptor set for the materialless path. Resolves
    // bindings against DrawBindings and PassResources only — no material
    // sources. Used during materialless preparation.
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
        size_t operator()(const PerPassCacheKey& key) const noexcept
        {
            using Rendering::HashUtils::HashCombine;
            uint64_t hash = key.ViewBits;
            hash = HashCombine(hash, key.PassKwBits);
            hash = HashCombine(hash, key.ContentsHash);
            hash = HashCombine(hash, key.SetIndex);
            hash = HashCombine(hash, key.PassInstanceIndex);
            return static_cast<size_t>(hash);
        }
    };

    // Missing-binding diagnostics are keyed by material and set on each preparation thread.
    struct WarnKey
    {
        const Material* SourceMaterial;
        uint32_t SetIndex;
        bool operator==(const WarnKey&) const = default;
    };
    struct WarnKeyHash
    {
        size_t operator()(const WarnKey& key) const noexcept
        {
            const uint64_t hash =
                Rendering::HashUtils::HashCombine(std::hash<const void*>{}(key.SourceMaterial), key.SetIndex);
            return static_cast<size_t>(hash);
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
    // The caller holds m_PerPassCacheMutex while searching immutable entries.
    const CachedSet* FindCachedSet(const PerPassCacheKey& key, const Rendering::DescriptorBindingPlan& plan,
                                  const std::vector<ResolvedBinding>& writes) const;
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
