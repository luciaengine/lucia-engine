#pragma once

// CBTResources — the persistent GPU buffer container for one CBT terrain
// instance. Owns the 14 SoA storage buffers, the descriptor set that binds them,
// and a host-visible readback buffer for debug counters. The device-local
// footprint scales with kDefaultBisectorPoolSize (SpecFor in CBTResources.cpp
// sizes each buffer); GetPersistentByteSize reports the actual total, which
// EnsureInitialized logs at bring-up.

#include <array>
#include <cstdint>
#include <span>

#include "CBTTerrain/CBTLayout.h"
#include "Rendering/Core/DeviceFrameCounter.h"
#include "Rendering/Core/Handle.h"

namespace GameEngine::Rendering
{
class IDevice;
}

namespace GameEngine::CBTTerrain
{

class CBTKernelSet;

class CBTResources
{
  public:
    CBTResources() = default;
    ~CBTResources();

    CBTResources(const CBTResources&) = delete;
    CBTResources& operator=(const CBTResources&) = delete;

    // Allocates every persistent buffer for `poolSize` bisectors and writes the
    // descriptor set from the kernel-set layout. `poolSize` must equal
    // kDefaultBisectorPoolSize for C1 (the sum-tree kernels bake its geometry).
    bool Initialize(Rendering::IDevice& device, const CBTKernelSet& kernelSet,
                    uint32_t poolSize = kDefaultBisectorPoolSize);
    void Shutdown();

    // Device-rebuild recovery. An in-place device rebuild already destroyed every
    // VkBuffer / VkImage / VkSampler behind these handles (VulkanDevice::
    // TeardownDeviceScopedForRebuild walks m_LiveBuffers / m_LiveTextures /
    // m_LiveSamplers), yet the handle members still read IsValid(). Forget them
    // WITHOUT calling Destroy* — Shutdown() would re-enter destruction on
    // already-freed handles — so the next Initialize allocates cleanly against the
    // rebuilt device. Mirrors the "drop the handles (no DestroyBuffer)" contract the
    // other re-provision consumers follow in RenderServices::OnDeviceRebuilt.
    void ReprovisionAfterDeviceRebuild();

    bool IsReady() const { return m_Ready; }
    uint32_t GetPoolSize() const { return m_PoolSize; }

    // The set a given kernel binds. Where every kernel shares one layout this is that one set
    // whatever the index; where each kernel carries only its own bindings (see
    // CBTKernelSet::KernelLayout) it is that kernel's matching set — WebGPU requires the bind
    // group and the pipeline's layout to agree exactly.
    Rendering::DescriptorSetHandle GetDescriptorSet(uint32_t kernelIndex = 0) const;
    Rendering::BufferHandle GetBuffer(CBTBinding binding) const;

    // Byte size of a storage buffer (binding). 0 for an out-of-range binding. Used to
    // bound debug readbacks against the source.
    uint64_t GetBufferByteSize(CBTBinding binding) const;

    // C3 indexed-indirect draw resources (not part of the kernel descriptor set —
    // consumed only by the graphics DrawIndexedIndirectCount). The identity index
    // buffer (index[i] = i, 3*P u32) makes gl_VertexIndex walk 0..indexCount-1; the
    // draw-count buffer holds the single-command count (1). Both are filled once by
    // CBTInstance::InitializeRoots and never rewritten. See CBTLayout.h §draw.
    Rendering::BufferHandle GetIdentityIndexBuffer() const { return m_IdentityIndexBuffer; }
    Rendering::BufferHandle GetDrawCountBuffer() const { return m_DrawCountBuffer; }

    // Host-visible readback buffer (large enough for the indirect-draw records
    // and validation counters copied for debug asserts).
    Rendering::BufferHandle GetReadbackBuffer() const { return m_Readback; }
    uint64_t GetReadbackBufferSize() const { return m_ReadbackSize; }

    // This frame's ring frame counter — unwrapped and monotonically increasing, the value
    // every per-frame entry point below reduces with CBTFrameRingSlot and the value
    // CBTInstance::RecordUpdate pushes for the GPU to reduce the same way. Call once per
    // device frame with IDevice::GetFrameIndex(), which is a CHANGE TOKEN here and never a
    // slot: its domain is [0, framesInFlight), too narrow to name this ring's last element,
    // so reducing its value directly would cap the rotation at the device's pacing and
    // strand that element on its Initialize-time default (see CBTFrameRingSlot for the
    // write-after-fence margin that costs). Idempotent within a device frame, so every view
    // declaring against one frame selects the same element and agrees with the push constant.
    uint32_t AdvanceFrameCounter(uint32_t deviceFrameIndex)
    {
        return m_FrameCounter.Tick(deviceFrameIndex);
    }

    /// Records the first submission that can read the source descriptor sets.
    void OnRootInitializationSubmitted() { m_SourceReadersSubmitted = true; }

    // C4 per-frame params UBO (binding 14) — camera + terrain params consumed by
    // Classify (screen-space error) and VertexEval (displacement). Host-visible,
    // ring-buffered over kCBTFrameParamsRing frames. Write one slot per frame.
    Rendering::BufferHandle GetFrameParamsBuffer() const { return m_FrameParams; }
    void UploadFrameParams(uint32_t frameCounter, const CBTFrameParams& params);

    // The current frame's ring element selects the height texture (binding 15).
    // Call source setters before recording. False leaves the requested binding unapplied.
    // Retires submitted graphics readers before changing the shared descriptor set.
    bool SetHeightSource(uint32_t frameCounter, Rendering::TextureHandle texture);
    Rendering::TextureHandle GetDefaultHeightTexture() const { return m_DefaultHeight; }
    // The height texture bound in `frameCounter`'s ring element (the flat default when none).
    Rendering::TextureHandle GetBoundHeightSource(uint32_t frameCounter) const;
    // The height-range pyramid (binding 21); invalid on the narrow-heap arm.
    Rendering::BufferHandle GetHeightRangeBuffer() const { return m_HeightRange; }
    // Compute binding 24 and graphics gCbtChain always bind a valid buffer.
    // Its zeroed header names no lattice until the wide heap provisions a chain.
    // The narrow heap keeps that header, so drawing uses cached corners.
    Rendering::BufferHandle GetHeightChainBuffer() const { return m_HeightChain; }
    uint32_t GetHeightChainWords() const { return m_HeightChainWords; }
    // Grow to CBTHeightChainWords for the lattice; never shrink. A new buffer has a zeroed header.
    // Allocation or completion failure, or an unsupported heap, preserves the old binding.
    bool ProvisionHeightChainWords(uint32_t words);
    // The far-field normal chain (binding 25 on the compute set as one storage view per mip,
    // cbt_NormalChain on the graphics set 2). A 1x1 placeholder until ProvisionNormalChain creates it for
    // a lattice; storage and sampled descriptors share GENERAL. Wide arm only, like the height chain.
    Rendering::TextureHandle GetNormalChainTexture() const { return m_NormalChain; }
    uint32_t GetNormalChainMips() const { return m_NormalChainMips; }
    /// True when the allocated normal chain matches this source lattice.
    bool HasNormalChainFor(uint32_t samplesX, uint32_t samplesZ) const
    {
        return samplesX >= 2u && samplesZ >= 2u && samplesX <= 0xFFFFu && samplesZ <= 0xFFFFu &&
               m_NormalChain.IsValid() && m_NormalChainLattice == (samplesX | (samplesZ << 16u));
    }
    // Creates the chain for a lattice of `samplesX` x `samplesZ` (CBTNormalChainTexels / Mips), replacing
    // one made for another lattice; true when it matches. False when the arm builds no chain or the
    // allocation or completion fails: the previous texture stays bound and the surface keeps the level-0 normal map.
    bool ProvisionNormalChain(uint32_t samplesX, uint32_t samplesZ);
    Rendering::SamplerHandle GetHeightSampler() const { return m_HeightSampler; }

    /// Restores source rings to their flat defaults before recording.
    /// Returns success; changed records applied updates even if a later update fails.
    bool RebindTerrainSourcesToDefault(bool& changed);

    // Monotonic version of the terrain-texture ring bindings (height/atlas/coarse). Bumped whenever
    // SetHeightSource/SetAtlasSource/SetCoarseSource actually change a slot, and by
    // RebindTerrainSourcesToDefault. The re-provision oracle asserts this advances in the same update
    // a retired heightmap is dropped, proving no ring element outlives the retired texture.
    uint64_t GetTerrainSourceGeneration() const { return m_TerrainSourceGeneration; }

    // Test/diagnostic: how many ring elements (across the height/atlas/coarse rings) currently bind
    // `texture`. The oracle asserts this is 0 for a retired texture after the rebind.
    uint32_t CountTerrainSourceSlots(Rendering::TextureHandle texture) const;

    // ---- Phase E resident-window atlas (bindings 17/18) ----
    //
    // Binding 18: rebind ONE ring element (this frame's, per CBTFrameRingSlot) to the
    // terrain's atlas height texture (or the flat default when invalid). Same in-flight
    // safety as SetHeightSource — only this frame's element is touched. No-op when that
    // element's handle is unchanged.
    bool SetAtlasSource(uint32_t frameCounter, Rendering::TextureHandle atlasTexture);
    // Binding 19: rebind the frame's coarse height field texture (out-of-window fallback), or
    // the flat default when invalid. Same per-slot in-flight safety as SetAtlasSource.
    bool SetCoarseSource(uint32_t frameCounter, Rendering::TextureHandle coarseTexture);
    // Binding 17: write `rowCount` 16-B indirection rows into the frame's ring slot from
    // the residency controller's table (host-visible; completes before submit, like the
    // params UBO). `rows` points at kAtlasRowBytes-sized TileAtlasSlot entries; the copy is
    // clamped to kAtlasMaxTiles. Rows past `rowCount` in the slot are zeroed so a shrunk
    // table never resolves a stale tile as resident.
    void UploadAtlasRows(uint32_t frameCounter, const void* rows, uint32_t rowCount);
    // ---- The paged height resolve (bindings 22/23, wide arm only; no-ops on the narrow arm) ----
    //
    // Binding 23: rebind this frame's ring element to the height page cache texture (or the flat
    // default when invalid). Same per-slot in-flight safety as SetAtlasSource.
    bool SetPageCacheSource(uint32_t frameCounter, Rendering::TextureHandle cacheTexture);
    // Binding 22: grows the page-table ring so a slot holds `words` (the bound terrain's table), in
    // steps of kCBTPageTableMinRingWords; the ring never shrinks. False when the allocation fails:
    // the previous ring stays bound and the terrain must not page this frame.
    bool ProvisionPageTableWords(uint32_t words);
    // Binding 22: write the terrain's page-table words (the CBTLayout.h binding-22 layout) into
    // this frame's ring slot, clamped to the slot. Empty words write an empty field.
    void UploadPageTable(uint32_t frameCounter, std::span<const uint32_t> words);
    uint32_t PageTableSlotWords() const { return m_PageTableSlotWords; }
    Rendering::TextureHandle GetDefaultAtlasHeightTexture() const { return m_DefaultAtlasHeight; }
    Rendering::TextureHandle GetDefaultAtlasCoarseTexture() const { return m_DefaultAtlasCoarse; }
    Rendering::TextureHandle GetDefaultPageCacheTexture() const { return m_DefaultPageCache; }
    Rendering::BufferHandle GetAtlasRowsBuffer() const { return m_AtlasRows; }

    // Sphere sculpt physical page POOL (binding 16) + page TABLE (binding 20) — planet editing v2.
    // Both host-visible SSBO rings: UploadSphereSculptPool / UploadSphereSculptPageTable write the
    // frame's ring slot from the caller's authoritative page store, so the slot the shader reads this
    // frame was written before this frame's GPU work (same ring discipline as UploadFrameParams — no
    // barrier/staging). Created zeroed (pool 0.0, table kSculptNoPage) so the default (no edits) reads
    // additive 0. The pool holds SculptPagePoolCount() pages; a page id addresses one 128^2 tile.
    //
    // Only the spherical domain reads them (every shader read is behind a spherical-domain gate), so
    // Initialize creates placeholders of kSculptPlaceholderSlotWords per ring slot that only keep
    // bindings 16 and 20 valid, and ProvisionSculptRings swaps in the full rings (64 MiB host-visible
    // at the default page budget) for a sphere and back for a planar tree. The uploads are no-ops on
    // the placeholders.
    void UploadSphereSculptPool(uint32_t frameCounter, const float* pool, uint32_t texelCount);
    void UploadSphereSculptPageTable(uint32_t frameCounter, const uint32_t* table, uint32_t entryCount);
    // Re-creates the two rings for the domain (true = spherical, with `pagePoolCount` physical pages:
    // ResolveSculptPagePoolCount in production; ignored for the placeholders) and rewrites the
    // descriptor sets. No-op when they already match. The new rings are created before the old ones
    // are destroyed: on an allocation failure it returns false with the previous rings, and the sets
    // that point at them, untouched. The caller guarantees no GPU work references the old rings
    // (CBTInstance::InitializeRoots first waits for prior graphics submissions).
    bool ProvisionSculptRings(bool spherical, uint32_t pagePoolCount);
    bool HasSphericalSculptRings() const { return m_SculptRingsSpherical; }
    uint32_t GetSculptPagePoolCount() const { return m_SculptPagePoolCount; }
    // One ring slot of each, in bytes: what the graphics side offset-binds per frame slot.
    uint64_t GetSculptPoolSlotBytes() const { return static_cast<uint64_t>(m_SculptPoolSlotWords) * sizeof(float); }
    uint64_t GetSculptTableSlotBytes() const { return static_cast<uint64_t>(m_SculptTableSlotWords) * sizeof(uint32_t); }
    // Both rings are bound to the GRAPHICS set 2 (by name) so the fragment can sample them for the
    // per-pixel sphere normal. Whole-buffer handles; the fragment offset-binds one ring slot.
    Rendering::BufferHandle GetSphereSculptBuffer() const { return m_SphereSculpt; }
    Rendering::BufferHandle GetSphereSculptPageTableBuffer() const { return m_SphereSculptTable; }

    // Planet-shading surface params (plan §planet-shading) — a small host-visible ring the
    // fragment reads (radius, relief amplitude/frequency/octaves, sculpt-enabled). Written
    // once per frame like the params UBO; offset-bound per slot on the graphics side.
    Rendering::BufferHandle GetSurfaceParamsBuffer() const { return m_SurfaceParams; }
    void UploadSurfaceParams(uint32_t frameCounter, const CBTSurfaceParams& params);

    // The draw's level rule (CBTDrawLevelParams), a host-visible uniform ring the vertex modifier reads
    // as gCbtDrawLevel; written once per frame, offset-bound per slot (kCBTDrawLevelParamsSlotStride).
    Rendering::BufferHandle GetDrawLevelParamsBuffer() const { return m_DrawLevelParams; }
    void UploadDrawLevelParams(uint32_t frameCounter, const CBTDrawLevelParams& params);

    // Total device-local footprint of the persistent buffers, in bytes.
    uint64_t GetPersistentByteSize() const { return m_PersistentBytes; }

  private:
    Rendering::BufferHandle CreateStorageBuffer(uint64_t size, uint32_t extraUsage, const char* name);
    Rendering::BufferHandle CreateHeightChain(uint32_t words);
    bool CreateNormalChain(uint32_t width, uint32_t height, uint32_t mips);
    void DestroyNormalChain();
    void WriteDescriptorSet();
    // One pair of sculpt rings and its shape, built before it replaces the bound pair.
    struct SculptRings
    {
        Rendering::BufferHandle Pool{};
        Rendering::BufferHandle Table{};
        uint32_t PagePoolCount = 0u;
        uint32_t PoolSlotWords = 0u;
        uint32_t TableSlotWords = 0u;
        uint64_t Bytes = 0;
        bool Spherical = false;
    };
    bool CreateSculptRings(bool spherical, uint32_t pagePoolCount, SculptRings& out);
    void AdoptSculptRings(const SculptRings& rings);
    void DestroySculptRings();

    Rendering::IDevice* m_Device = nullptr;
    // Unwraps the device's wrapped frame slot so all kCBTFrameParamsRing elements rotate
    // (see AdvanceFrameCounter). Lives with the rings it indexes.
    Rendering::DeviceFrameCounter m_FrameCounter;
    uint32_t m_PoolSize = 0;
    std::array<Rendering::BufferHandle, kCBTBindingCount> m_Buffers{};
    Rendering::BufferHandle m_IdentityIndexBuffer{};
    Rendering::BufferHandle m_DrawCountBuffer{};
    Rendering::BufferHandle m_FrameParams{};       // UBO ring (binding 14)
    Rendering::BufferHandle m_SphereSculpt{};      // host-visible SSBO ring: page pool (binding 16)
    Rendering::BufferHandle m_SphereSculptTable{}; // host-visible SSBO ring: page table (binding 20)
    uint32_t m_SculptPagePoolCount = 0u;           // physical page count (ResolveSculptPagePoolCount); 0 on the placeholders
    uint32_t m_SculptPoolSlotWords = 0u;           // floats per pool ring slot
    uint32_t m_SculptTableSlotWords = 0u;          // entries per page-table ring slot
    uint64_t m_SculptRingBytes = 0;                // both rings, all slots
    bool m_SculptRingsSpherical = false;           // false = the placeholders
    Rendering::BufferHandle m_SurfaceParams{};     // host-visible ring, graphics set 2 (plan §planet-shading)
    Rendering::TextureHandle m_DefaultHeight{};    // 1x1 flat fallback heightmap
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundHeight{}; // per-ring-slot bound texture (binding 15)
    Rendering::SamplerHandle m_HeightSampler{};    // linear-clamp, matches CDLOD's GE_TS_CLAMP
    Rendering::BufferHandle m_AtlasRows{};         // host-visible SSBO ring (binding 17)
    Rendering::BufferHandle m_HeightRange{};       // device-local height-range pyramid (binding 21)
    Rendering::BufferHandle m_HeightChain{};       // device-local prefiltered height chain (binding 24)
    uint32_t m_HeightChainWords = 0u;              // words m_HeightChain holds
    Rendering::BufferHandle m_DrawLevelParams{};   // host-visible ring, graphics set 2 (gCbtDrawLevel)
    Rendering::TextureHandle m_NormalChain{};      // far-field normal chain, rg16f with mips (binding 25)
    std::array<Rendering::TextureViewHandle, kCBTNormalChainMaxMips> m_NormalChainViews{}; // one per mip
    uint32_t m_NormalChainMips = 0u;
    uint32_t m_NormalChainLattice = 0u;            // samples x | z << 16 the chain was made for
    Rendering::TextureHandle m_DefaultAtlasHeight{}; // 1x1 flat fallback atlas height
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundAtlasHeight{}; // per-ring-slot atlas texture (binding 18)
    Rendering::TextureHandle m_DefaultAtlasCoarse{}; // 1x1 flat fallback coarse field
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundAtlasCoarse{}; // per-ring-slot coarse field (binding 19)
    Rendering::BufferHandle m_PageTable{};           // host-visible page-table ring (binding 22), wide arm only
    uint32_t m_PageTableSlotWords = 0;               // words per ring slot of m_PageTable
    Rendering::TextureHandle m_DefaultPageCache{};   // 1x1 flat page cache (binding 23), wide arm only
    std::array<Rendering::TextureHandle, kCBTTextureRingMax> m_BoundPageCache{}; // per-ring-slot page cache (binding 23)
    uint32_t m_TextureRing = kCBTTextureRingMax; // elements the loaded kernels declare per texture
    // One shared set where the device binds the whole family's storage buffers in one
    // stage; one set per kernel where it cannot (CBTKernelSet::UsesPerKernelLayouts).
    bool m_SourceReadersSubmitted = false;
    Rendering::DescriptorSetHandle m_DescriptorSet{};
    std::vector<Rendering::DescriptorSetHandle> m_KernelSets;
    std::vector<std::vector<uint32_t>> m_KernelSetBindings;
    static size_t PageTableRingBytes(uint32_t slotWords);
    Rendering::BufferHandle CreatePageTableRing(uint32_t slotWords);
    void WriteOneSet(Rendering::DescriptorSetHandle set, const std::vector<uint32_t>& only);
    // Rewrites one ring element of a terrain-source binding in every set that
    // binds it (the per-kernel sets, or the single shared set).
    bool WaitForSourceReaders() const;
    bool WaitForGraphicsReaders() const;
    bool RebindSourceRingToDefault(std::array<Rendering::TextureHandle, kCBTTextureRingMax>& ring,
                                    Rendering::TextureHandle texture, uint32_t binding, bool& changed);
    bool UpdateTerrainSourceBinding(uint32_t binding, Rendering::TextureHandle texture,
                                    uint32_t slot);
    Rendering::BufferHandle m_Readback{};
    uint64_t m_ReadbackSize = 0;
    uint64_t m_PersistentBytes = 0;
    // Binding version of the terrain-texture rings (height 15 / atlas 18 / coarse 19). See
    // GetTerrainSourceGeneration — the re-provision drop advances it, and the oracle keys on it.
    uint64_t m_TerrainSourceGeneration = 0;
    bool m_Ready = false;
};

} // namespace GameEngine::CBTTerrain
