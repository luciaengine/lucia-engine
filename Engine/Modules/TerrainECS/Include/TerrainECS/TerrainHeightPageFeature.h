#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "Types/FlatMap.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;
class RGPassBuilder;
} // namespace GameEngine::Rendering::RenderGraph

namespace GameEngine::TerrainECS
{

/// Owns the shared GPU height cache. Tables outside the upload's graph name submitted payloads.
/// Inside that graph, readers declared after its copy can use the staged table.
class TerrainHeightPageFeature : public Engine::Renderer::IRenderFeature
{
public:
    ~TerrainHeightPageFeature() override;

    /// Main thread: takes pending page payloads and copies changed table words.
    void Publish(TerrainHeightPages& pages);
    /// Render thread: latches published data and prepares retryable staging for the upload stream.
    void Latch(Rendering::IDevice& device, uint64 rgFrame);
    bool HasStagedUploads() const { return m_Staging.IsValid(); }
    /// Declares the cache overwrite and promotes its tables only after the recorded copy submits.
    void DeclareUploads(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGPassBuilder& pass);
    /// Records copies in the graph's CopyDest layout. Payloads remain pending until submission.
    void FlushUploads(Rendering::CommandList& commandList, const Rendering::RenderGraph::RGFrame& frame);

    /// Render thread: cache visible to this declaration, or the last submitted cache without a graph.
    Rendering::TextureHandle CacheTexture(const Rendering::RenderGraph::RGFrame* frame = nullptr) const;
    /// Render thread: table visible to this declaration and its payload generation.
    bool LatchedTable(uint32 terrain, std::span<const uint32>& words, uint64& version,
                      uint64& contentGeneration, const Rendering::RenderGraph::RGFrame* frame = nullptr, uint64* revision = nullptr) const;
    /// Render thread: bounds proven to cover the consumer's interval in this selected snapshot.
    bool DirtyRegionSince(uint32 terrain, uint32 terrainGeneration, uint64 consumedRevision,
                          uint64 selectedRevision, const Rendering::RenderGraph::RGFrame* frame,
                          HeightPageDirtyRect& region) const;
    /// Retires accumulated bounds only when this exact geometry publication caught up.
    void AcknowledgePublication(uint32 terrain, uint64 revision);
    /// Any thread: requested data differs from the publication consumed by a terrain update.
    bool HasPendingPublication(uint32 terrain, uint64 version, uint64 contentGeneration) const;
    uint64 DeviceEpoch() const { return m_DeviceEpoch.load(std::memory_order_acquire); }

    struct TerrainReadiness
    {
        uint32 Terrain = 0;
        uint64 RequestedVersion = 0;
        uint64 CommittedVersion = 0;
        uint64 RequestedContentGeneration = 0;
        uint64 CommittedContentGeneration = 0;
    };
    struct Readiness
    {
        uint32 Slots = 0;
        uint32 Resident = 0;
        uint32 Deferred = 0;
        uint32 LoadsStarted = 0;
        uint64 CpuBytes = 0;
        uint64 CacheBytes = 0;
        uint64 PublishedUploads = 0;
        uint64 LatchedUploads = 0;
        uint64 PendingBytes = 0;
        uint64 RetiringBytes = 0;
        uint64 LatchedFrame = ~0ull;
        uint64 SubmittedFrame = ~0ull;
        bool CacheReady = false;
        bool StagingReady = false;
        std::vector<TerrainReadiness> Terrains;
    };
    /// Any thread: copied readiness counters; the shared cache totals appear once.
    Readiness GetReadiness() const;

    void OnDeviceRebuilt(Rendering::IDevice* device) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;

private:
    struct Table
    {
        std::shared_ptr<const std::vector<uint32>> Words;
        uint64 Version = 0;
        uint64 ContentGeneration = 0;
        uint64 Revision = 0;
        uint64 DirtyBaseRevision = 0;
        HeightPageDirtyRect Dirty;
        bool DirtyPending = false;
        bool DirtyFullRequired = false;
        bool Changed = false;
    };
    struct RetiringBuffer
    {
        Rendering::BufferHandle Buffer;
        Rendering::IDevice::GpuSyncToken Submission;
        const Rendering::RenderGraph::RGFrame* Frame = nullptr;
        uint64 Bytes = 0;
    };

    bool HasPlannedUploads(const Rendering::RenderGraph::RGFrame* frame) const;
    void EnsureCache();
    void StageUploads();
    void SubmitUploads(Rendering::RenderGraph::RGFrame& frame);
    void UpdateReadiness();

    mutable std::mutex m_Mutex;
    Readiness m_Readiness;
    FlatMap<uint32, HeightPageUpload> m_PublishedUploads;
    FlatMap<uint32, Table> m_PublishedTables;
    uint32 m_CacheDim = 0;
    uint64 m_ContentGeneration = 0;
    uint64 m_PublicationRevision = 0; // never reset across device/source incarnations

    Rendering::IDevice* m_Device = nullptr;
    FlatMap<uint32, HeightPageUpload> m_LatchedUploads;
    FlatMap<uint32, Table> m_LatchedTables;
    FlatMap<uint32, Table> m_CommittedTables;
    Rendering::BufferHandle m_Staging;
    std::vector<RetiringBuffer> m_Retiring;
    Rendering::TextureHandle m_Cache;
    const Rendering::RenderGraph::RGFrame* m_UploadGraph = nullptr;
    uint64 m_UploadFrame = ~0ull;
    bool m_UploadRecorded = false;
    bool m_CacheCleared = false;
    uint32 m_LatchedCacheDim = 0;
    uint64 m_LatchedFrame = ~0ull;
    std::atomic<uint64> m_DeviceEpoch{0};
};

} // namespace GameEngine::TerrainECS
