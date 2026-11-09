#include "TerrainECS/TerrainHeightPageFeature.h"

#include "Logger/Logger.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

namespace
{

constexpr std::size_t kPageBytes = static_cast<std::size_t>(PageStreaming::kPageSampleCount) * sizeof(float32);

} // namespace

TerrainHeightPageFeature::~TerrainHeightPageFeature()
{
    if (!m_Device)
        return;
    for (const RetiringBuffer& retiring : m_Retiring)
        m_Device->DestroyBuffer(retiring.Buffer);
    if (m_Staging.IsValid())
        m_Device->DestroyBuffer(m_Staging);
    if (m_Cache.IsValid())
        m_Device->DestroyTexture(m_Cache);
}

void TerrainHeightPageFeature::Publish(TerrainHeightPages& pages)
{
    std::vector<uint32> paged;
    pages.PagedTerrains(paged);
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CacheDim = pages.Geometry().CacheDim;
    const auto& residency = pages.Residency();
    const auto& cache = residency.FieldCache(kHeightPageField);
    m_Readiness.Slots = pages.SlotCount();
    m_Readiness.Resident = cache.ResidentCount();
    m_Readiness.Deferred = cache.DeferredThisFrame();
    m_Readiness.LoadsStarted = residency.LoadsStartedThisFrame();
    m_Readiness.CpuBytes = residency.CpuBytes();
    for (auto& [terrain, table] : m_PublishedTables)
        table.Changed = false;
    for (HeightPageUpload& upload : pages.Uploads())
    {
        Table& table = m_PublishedTables.GetOrInsert(upload.Terrain);
        table.ContentGeneration = ++m_ContentGeneration;
        table.Changed = true;
        m_PublishedUploads.InsertOrAssign(upload.Slot, std::move(upload));
    }
    pages.Uploads().clear();
    m_PublishedTables.EraseIf([&paged](uint32 terrain, const Table&) {
        return std::find(paged.begin(), paged.end(), terrain) == paged.end();
    });
    for (uint32 terrain : paged)
    {
        uint64 version = 0;
        const std::vector<uint32>* words = pages.TableWords(terrain, version);
        Table& table = m_PublishedTables.GetOrInsert(terrain);
        if (words == nullptr || (table.Version == version && table.Words))
            continue;
        table.Words = std::make_shared<const std::vector<uint32>>(*words);
        table.Version = version;
        table.Changed = true;
    }
    for (auto& [terrain, table] : m_PublishedTables)
    {
        if (!table.Changed)
            continue;
        if (!table.DirtyPending)
        {
            table.DirtyBaseRevision = table.Revision;
            table.Dirty = {};
            table.DirtyFullRequired = false;
        }
        bool found = false;
        for (const HeightPageDirtyRect& region : pages.DirtyRects())
        {
            if (region.Terrain != terrain)
                continue;
            if (!std::isfinite(region.MinU) || !std::isfinite(region.MinV) ||
                !std::isfinite(region.MaxU) || !std::isfinite(region.MaxV) ||
                region.MinU < 0.0f || region.MinV < 0.0f || region.MaxU > 1.0f || region.MaxV > 1.0f ||
                region.MinU >= region.MaxU || region.MinV >= region.MaxV)
            {
                table.DirtyFullRequired = true;
                continue;
            }
            const bool hadBounds = table.Dirty.MaxU > table.Dirty.MinU && table.Dirty.MaxV > table.Dirty.MinV;
            if (hadBounds && table.Dirty.TerrainGeneration != region.TerrainGeneration)
                table.DirtyFullRequired = true;
            if (!hadBounds || table.Dirty.TerrainGeneration != region.TerrainGeneration)
                table.Dirty = region;
            else
            {
                table.Dirty.MinU = std::min(table.Dirty.MinU, region.MinU);
                table.Dirty.MinV = std::min(table.Dirty.MinV, region.MinV);
                table.Dirty.MaxU = std::max(table.Dirty.MaxU, region.MaxU);
                table.Dirty.MaxV = std::max(table.Dirty.MaxV, region.MaxV);
            }
            found = true;
        }
        table.DirtyFullRequired |= !found;
        table.DirtyPending = true;
        table.Revision = ++m_PublicationRevision;
    }
}

void TerrainHeightPageFeature::Latch(Rendering::IDevice& device, uint64 rgFrame)
{
    if (rgFrame == m_LatchedFrame)
        return;
    m_LatchedFrame = rgFrame;
    m_Device = &device;
    m_UploadGraph = nullptr;
    m_UploadRecorded = false;
    std::erase_if(m_Retiring, [&device](const RetiringBuffer& retiring) {
        if (device.QueryGpuSyncToken(retiring.Submission) != Rendering::IDevice::GpuSyncStatus::Complete)
            return false;
        device.DestroyBuffer(retiring.Buffer);
        return true;
    });

    bool newUploads = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        newUploads = !m_PublishedUploads.Empty();
        for (auto& [slot, upload] : m_PublishedUploads)
            m_LatchedUploads.InsertOrAssign(slot, std::move(upload));
        m_PublishedUploads.Clear();
        m_LatchedTables = m_PublishedTables;
        m_LatchedCacheDim = m_CacheDim;
    }
    if (m_LatchedUploads.Empty())
    {
        m_CommittedTables = m_LatchedTables;
        UpdateReadiness();
        return;
    }
    if (newUploads || !m_Staging.IsValid())
        StageUploads();
    UpdateReadiness();
}

void TerrainHeightPageFeature::StageUploads()
{
    // This staging has not submitted. Submitted buffers move to m_Retiring.
    if (m_Staging.IsValid())
        m_Device->DestroyBuffer(m_Staging);
    m_Staging = {};
    EnsureCache();
    if (!m_Cache.IsValid())
        return;
    const std::size_t bytes = m_LatchedUploads.Size() * kPageBytes;
    m_Staging = m_Device->CreateUploadBuffer(bytes, "Terrain.HeightPageStaging");
    if (!m_Staging.IsValid())
    {
        Logger::Log::Warning("Terrain pages: no staging for {} page uploads ({} bytes); the page tables keep their "
                             "last uploaded state and the uploads retry next frame",
                             m_LatchedUploads.Size(), bytes);
        return;
    }
    std::size_t offset = 0;
    for (const auto& [slot, upload] : m_LatchedUploads)
    {
        m_Device->UpdateBuffer(m_Staging, offset, kPageBytes, upload.Samples.data());
        offset += kPageBytes;
    }
}

void TerrainHeightPageFeature::DeclareUploads(Rendering::RenderGraph::RGFrame& frame,
                                              Rendering::RenderGraph::RGPassBuilder& pass)
{
    if (!HasStagedUploads() || !m_Cache.IsValid())
        return;
    m_UploadGraph = &frame;
    m_UploadFrame = frame.FrameIndex();
    m_UploadRecorded = false;
    const auto cache = frame.ImportExternalTexture("Terrain.HeightPageCache", m_Cache,
        m_CacheCleared ? Rendering::ResourceState::ShaderResource : Rendering::ResourceState::Undefined);
    pass.Write(cache, Rendering::RenderGraph::RGTextureWrite::CopyDst);
    pass.Prepare([this, graph = &frame, frameIndex = m_UploadFrame, staging = m_Staging]
                 (Rendering::RenderGraph::RGPrepareContext&)
    {
        return m_UploadGraph == graph && m_UploadFrame == frameIndex && m_Staging == staging;
    });
    frame.MarkOutput(cache, Rendering::RenderGraph::RGImageLayout::ShaderReadOnly);
    pass.OnSubmitted([this, graph = &frame, frameIndex = frame.FrameIndex()]
    {
        if (m_UploadGraph == graph && m_UploadFrame == frameIndex)
            SubmitUploads(*graph);
    });
}

void TerrainHeightPageFeature::SubmitUploads(Rendering::RenderGraph::RGFrame& frame)
{
    if (!m_UploadRecorded)
        return;
    m_CommittedTables = m_LatchedTables;
    m_CacheCleared = true;
    m_Retiring.push_back({m_Staging, frame.SubmissionToken(), &frame, m_LatchedUploads.Size() * kPageBytes});
    m_Staging = {};
    m_LatchedUploads.Clear();
    m_UploadRecorded = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Readiness.SubmittedFrame = m_UploadFrame;
    }
    UpdateReadiness();
}

void TerrainHeightPageFeature::EnsureCache()
{
    if (m_Cache.IsValid() || m_LatchedCacheDim == 0)
        return;
    Rendering::TextureDesc description{};
    description.width = m_LatchedCacheDim;
    description.height = m_LatchedCacheDim;
    description.format = static_cast<uint32>(Rendering::TextureFormat::R32_FLOAT);
    description.usage = static_cast<uint32>(Rendering::TextureUsage::ShaderResource) |
                        static_cast<uint32>(Rendering::TextureUsage::TransferDst);
    description.persistent = true;
    description.debugName = "Terrain.HeightPageCache";
    m_Cache = m_Device->CreateTexture(description);
    m_CacheCleared = false;
}

void TerrainHeightPageFeature::FlushUploads(Rendering::CommandList& commandList,
                                            const Rendering::RenderGraph::RGFrame& frame)
{
    if (!m_Staging.IsValid() || !m_Cache.IsValid() || !HasPlannedUploads(&frame))
        return;
    if (!m_CacheCleared)
    {
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        commandList.ClearColorImageSubresource(m_Cache, 0, 0, zero);
    }
    const uint32 slotsPerRow = m_LatchedCacheDim / PageStreaming::kPageStrideSamples;
    std::size_t offset = 0;
    for (const auto& [slot, upload] : m_LatchedUploads)
    {
        commandList.CopyBufferToTextureSubresource(m_Staging, m_Cache, 0, 0, PageStreaming::kPageStrideSamples,
            PageStreaming::kPageStrideSamples, offset, PageStreaming::kPageStrideSamples * sizeof(float32), 1, 0,
            (slot % slotsPerRow) * PageStreaming::kPageStrideSamples,
            (slot / slotsPerRow) * PageStreaming::kPageStrideSamples, Rendering::ResourceState::CopyDest);
        offset += kPageBytes;
    }
    m_UploadRecorded = true;
}

bool TerrainHeightPageFeature::HasPlannedUploads(const Rendering::RenderGraph::RGFrame* frame) const
{
    return frame != nullptr && frame == m_UploadGraph && frame->FrameIndex() == m_UploadFrame;
}

Rendering::TextureHandle TerrainHeightPageFeature::CacheTexture(const Rendering::RenderGraph::RGFrame* frame) const
{
    return m_CacheCleared || HasPlannedUploads(frame) ? m_Cache : Rendering::TextureHandle{};
}

bool TerrainHeightPageFeature::LatchedTable(uint32 terrain, std::span<const uint32>& words, uint64& version,
                                           uint64& contentGeneration,
                                           const Rendering::RenderGraph::RGFrame* frame, uint64* revision) const
{
    const Table* table = HasPlannedUploads(frame) ? m_LatchedTables.Find(terrain) : m_CommittedTables.Find(terrain);
    if (table == nullptr || !table->Words || table->Words->empty())
        return false;
    words = *table->Words;
    version = table->Version;
    contentGeneration = table->ContentGeneration;
    if (revision != nullptr)
        *revision = table->Revision;
    return true;
}

bool TerrainHeightPageFeature::DirtyRegionSince(uint32 terrain, uint32 terrainGeneration, uint64 consumedRevision,
                                               uint64 selectedRevision,
                                               const Rendering::RenderGraph::RGFrame* frame,
                                               HeightPageDirtyRect& region) const
{
    const Table* table = HasPlannedUploads(frame) ? m_LatchedTables.Find(terrain) : m_CommittedTables.Find(terrain);
    if (table == nullptr || !table->DirtyPending || table->DirtyFullRequired ||
        table->Revision != selectedRevision || consumedRevision == 0 ||
        consumedRevision < table->DirtyBaseRevision || consumedRevision >= selectedRevision ||
        table->Dirty.TerrainGeneration != terrainGeneration)
        return false;
    region = table->Dirty;
    return true;
}

void TerrainHeightPageFeature::AcknowledgePublication(uint32 terrain, uint64 revision)
{
    if (revision == 0)
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    Table* table = m_PublishedTables.Find(terrain);
    if (table != nullptr && table->Revision == revision)
    {
        table->DirtyBaseRevision = revision;
        table->DirtyPending = false;
        table->DirtyFullRequired = false;
        table->Dirty = {};
    }
}

bool TerrainHeightPageFeature::HasPendingPublication(uint32 terrain, uint64 version, uint64 contentGeneration) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const Table* requested = m_PublishedTables.Find(terrain);
    return requested != nullptr && (requested->Version != version || requested->ContentGeneration != contentGeneration);
}

TerrainHeightPageFeature::Readiness TerrainHeightPageFeature::GetReadiness() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    Readiness snapshot = m_Readiness;
    snapshot.PublishedUploads = m_PublishedUploads.Size();
    snapshot.PendingBytes = (snapshot.PublishedUploads + snapshot.LatchedUploads) * kPageBytes;
    snapshot.Terrains.clear();
    for (const auto& [terrain, requested] : m_PublishedTables)
    {
        TerrainReadiness entry{terrain, requested.Version, 0, requested.ContentGeneration, 0};
        const auto submitted = std::find_if(m_Readiness.Terrains.begin(), m_Readiness.Terrains.end(),
            [terrain](const TerrainReadiness& state) { return state.Terrain == terrain; });
        if (submitted != m_Readiness.Terrains.end())
        {
            entry.CommittedVersion = submitted->CommittedVersion;
            entry.CommittedContentGeneration = submitted->CommittedContentGeneration;
        }
        snapshot.Terrains.push_back(entry);
    }
    return snapshot;
}

void TerrainHeightPageFeature::UpdateReadiness()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Readiness.CacheBytes = m_Cache.IsValid() ? uint64{m_LatchedCacheDim} * m_LatchedCacheDim * sizeof(float32) : 0;
    m_Readiness.CacheReady = m_CacheCleared;
    m_Readiness.StagingReady = m_Staging.IsValid();
    m_Readiness.LatchedFrame = m_LatchedFrame;
    m_Readiness.LatchedUploads = m_LatchedUploads.Size();
    m_Readiness.RetiringBytes = 0;
    for (const RetiringBuffer& buffer : m_Retiring)
        m_Readiness.RetiringBytes += buffer.Bytes;
    m_Readiness.Terrains.clear();
    for (const auto& [terrain, table] : m_CommittedTables)
        m_Readiness.Terrains.push_back({terrain, 0, table.Version, 0, table.ContentGeneration});
}

void TerrainHeightPageFeature::OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame)
{
    std::erase_if(m_Retiring, [this, &frame](const RetiringBuffer& buffer)
    {
        if (buffer.Frame != &frame)
            return false;
        if (!m_Device->WaitGpuSyncToken(buffer.Submission))
            m_Device->WaitForIdle();
        m_Device->DestroyBuffer(buffer.Buffer);
        return true;
    });
    if (m_UploadGraph == &frame)
    {
        m_UploadGraph = nullptr;
        m_UploadFrame = ~0ull;
        m_LatchedFrame = ~0ull;
        m_UploadRecorded = false;
    }
    UpdateReadiness();
}

void TerrainHeightPageFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    // The old device owns destruction. Forget every handle and retry from the new cache epoch.
    m_Device = device;
    m_Cache = {};
    m_CacheCleared = false;
    m_Staging = {};
    m_Retiring.clear();
    m_LatchedUploads.Clear();
    m_LatchedTables.Clear();
    m_CommittedTables.Clear();
    m_UploadGraph = nullptr;
    m_UploadRecorded = false;
    m_LatchedFrame = ~0ull;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PublishedUploads.Clear();
        for (auto& [terrain, table] : m_PublishedTables)
        {
            table.DirtyBaseRevision = table.Revision;
            table.Revision = ++m_PublicationRevision;
            table.DirtyPending = true;
            table.DirtyFullRequired = true;
        }
        m_Readiness = {};
    }
    m_DeviceEpoch.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace GameEngine::TerrainECS
