#include "TerrainECS/TerrainPageFeature.h"

#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace GameEngine::TerrainECS
{

namespace
{

constexpr std::size_t kPageBytes = static_cast<std::size_t>(PageStreaming::kPageSampleCount) * sizeof(float32);
// A surface upload's bytes start on a 16-byte boundary in the staging: a BC5 copy's buffer offset is a
// multiple of its 16-byte block, and an RG8 plane (33,800 bytes) breaks that alignment for the planes after it.
constexpr std::size_t kSurfaceStagingAlignment = PageStreaming::kBc5BlockBytes;
// The released snapshots a table keeps: the published, latched and committed sets hold the others.
constexpr std::size_t kMaxFreeSnapshots = 2u;

constexpr std::array<const char*, kSurfacePlanes.size()> kPlaneCacheNames = {
    "Terrain.SurfacePageCache.Splat", "Terrain.SurfacePageCache.Normal", "Terrain.SurfacePageCache.GrassControl",
    "Terrain.SurfacePageCache.Color"};

// The feature owns its cache textures: the device frees each one after the frames in flight.
void DestroyValidTextures(Rendering::IDevice& device, std::span<const Rendering::TextureHandle> textures)
{
    for (const Rendering::TextureHandle texture : textures)
        if (texture.IsValid())
            device.DestroyTexture(texture);
}

Rendering::TextureFormat PlaneTextureFormat(PageStreaming::PageFieldFormat format)
{
    switch (format)
    {
    case PageStreaming::PageFieldFormat::Bc5: return Rendering::TextureFormat::BC5_UNORM;
    case PageStreaming::PageFieldFormat::Rg8: return Rendering::TextureFormat::R8G8_UNORM;
    case PageStreaming::PageFieldFormat::Rgba8Srgb: return Rendering::TextureFormat::RGBA8_SRGB;
    default: return Rendering::TextureFormat::RGBA8_UNORM;
    }
}

// The cells of a grid of `cells` a side over terrain UV that rect [minU, maxU] x [minV, maxV] meets.
struct GridCells
{
    uint32 X0 = 0;
    uint32 X1 = 0;
    uint32 Z0 = 0;
    uint32 Z1 = 0;
};

GridCells CellsOf(const Mathematics::Rect& uv, uint32 cells)
{
    const float32 extent = static_cast<float32>(cells);
    const auto cell = [extent](float32 t) { return static_cast<uint32>(std::clamp(t * extent, 0.0f, extent - 1.0f)); };
    return GridCells{cell(uv.X), cell(uv.Right()), cell(uv.Y), cell(uv.Bottom())};
}

} // namespace

TerrainPageFeature::~TerrainPageFeature()
{
    if (!m_Device)
        return;
    for (const RetiringBuffer& retiring : m_Retiring)
        m_Device->DestroyBuffer(retiring.Buffer);
    m_TableRing.Release(m_Device);
    if (m_Staging.IsValid())
        m_Device->DestroyBuffer(m_Staging);
    std::array<Rendering::TextureHandle, kSurfacePlanes.size() + 1u> textures{m_Cache};
    for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
        textures[plane + 1u] = m_Planes[plane].Texture;
    DestroyValidTextures(*m_Device, textures);
}

void TerrainPageFeature::PublishTable(Table& table, const PackedPageTable* source, uint32 planeMask)
{
    if (source == nullptr || (table.Snapshot && table.Snapshot->Version() == source->Version()))
        return;
    if (!table.Pool)
        table.Pool = std::make_shared<TablePool>();
    std::unique_ptr<PackedPageTable> snapshot;
    {
        std::lock_guard<std::mutex> lock(table.Pool->Mutex);
        if (!table.Pool->Free.empty())
        {
            snapshot = std::move(table.Pool->Free.back());
            table.Pool->Free.pop_back();
        }
    }
    if (!snapshot)
        snapshot = std::make_unique<PackedPageTable>();
    snapshot->CopyFrom(*source);
    table.Snapshot = std::shared_ptr<const PackedPageTable>(
        snapshot.release(), [pool = table.Pool](const PackedPageTable* released) { ReturnSnapshot(pool, released); });
    table.PlaneMask = planeMask;
    table.Version = source->Version();
    table.Changed = true;
}

void TerrainPageFeature::ReturnSnapshot(const std::shared_ptr<TablePool>& pool, const PackedPageTable* snapshot)
{
    // Publish made the snapshot non-const; only the published sets see it as const.
    std::unique_ptr<PackedPageTable> owned(const_cast<PackedPageTable*>(snapshot));
    std::lock_guard<std::mutex> lock(pool->Mutex);
    if (pool->Free.size() < kMaxFreeSnapshots)
        pool->Free.push_back(std::move(owned));
}

void TerrainPageFeature::ForgetUnpaged(TableMap& tables, std::span<const uint32> paged)
{
    tables.EraseIf([&paged](uint32 terrain, const Table&)
                   { return std::find(paged.begin(), paged.end(), terrain) == paged.end(); });
}

void TerrainPageFeature::Publish(TerrainPages& pages)
{
    std::vector<uint32> paged;
    std::vector<uint32> surfacePaged;
    pages.PagedTerrains(paged);
    TerrainSurfacePages& surface = pages.Surface();
    surface.PagedTerrains(surfacePaged);
    if (m_ReportRequested.exchange(false, std::memory_order_acq_rel))
        PublishTableHashes(pages, paged);
    GE_CPU_PROFILE_SCOPE("Terrain.PageTable.Publish");
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CacheDim = pages.Geometry().CacheDim;
    const uint32 surfaceSlotsPerRow = surface.Geometry().SlotsPerRow;
    if (m_SurfaceSlotsPerRow != surfaceSlotsPerRow)
        m_PublishedSurfaceUploads.Clear();
    m_SurfaceSlotsPerRow = surfaceSlotsPerRow;
    const auto& residency = pages.Residency();
    const auto& heightCache = residency.FieldCache(kHeightPageField);
    m_Readiness.Slots = pages.SlotCount();
    m_Readiness.Resident = heightCache.ResidentCount();
    m_Readiness.Deferred = heightCache.DeferredThisFrame();
    m_Readiness.LoadsStarted = residency.LoadsStartedThisFrame();
    m_Readiness.CpuBytes = residency.CpuBytes();
    for (auto& [terrain, table] : m_PublishedTables.Height)
        table.Changed = false;
    for (HeightPageUpload& upload : pages.Uploads())
    {
        Table& table = m_PublishedTables.Height.GetOrInsert(upload.Terrain);
        table.ContentGeneration = ++m_ContentGeneration;
        table.Changed = true;
        m_PublishedUploads.InsertOrAssign(upload.Slot, std::move(upload));
    }
    pages.Uploads().clear();
    for (SurfacePageUpload& upload : surface.Uploads())
    {
        m_PublishedTables.Surface.GetOrInsert(upload.Terrain).ContentGeneration = ++m_ContentGeneration;
        m_PublishedSurfaceUploads.InsertOrAssign(upload.Slot, std::move(upload));
    }
    surface.Uploads().clear();
    m_PublishedChanges.insert(m_PublishedChanges.end(), pages.ChangeRects().begin(), pages.ChangeRects().end());
    pages.ChangeRects().clear();
    m_PublishedChanges.insert(m_PublishedChanges.end(), surface.ChangeRects().begin(), surface.ChangeRects().end());
    surface.ChangeRects().clear();
    ForgetUnpaged(m_PublishedTables.Height, paged);
    ForgetUnpaged(m_PublishedTables.Surface, surfacePaged);
    for (uint32 terrain : paged)
        PublishTable(m_PublishedTables.Height.GetOrInsert(terrain), pages.PackedTable(terrain), 0u);
    for (uint32 terrain : surfacePaged)
    {
        uint32 planeMask = 0;
        const PackedPageTable* snapshot = surface.PackedTable(terrain, planeMask);
        PublishTable(m_PublishedTables.Surface.GetOrInsert(terrain), snapshot, planeMask);
    }
    for (auto& [terrain, table] : m_PublishedTables.Height)
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

void TerrainPageFeature::Latch(Rendering::IDevice& device, uint64 rgFrame)
{
    if (rgFrame == m_LatchedFrame)
        return;
    m_LatchedFrame = rgFrame;
    m_Device = &device;
    m_UploadGraph = nullptr;
    m_UploadRecorded = false;
    m_RecordedPlanes = {};
    m_CacheReadable = m_Cache.IsValid() && m_CacheCleared;
    for (PlaneCache& plane : m_Planes)
        plane.Readable = plane.Texture.IsValid() && plane.Written;

    const uint64 retireAfter = static_cast<uint64>(device.GetFramesInFlight()) + 1u;
    std::erase_if(m_Retiring, [&](const RetiringBuffer& retiring) {
        if (retiring.Submission.IsValid())
        {
            if (device.QueryGpuSyncToken(retiring.Submission) != Rendering::IDevice::GpuSyncStatus::Complete)
                return false;
        }
        else if (rgFrame < retiring.Frame + retireAfter)
            return false;
        device.DestroyBuffer(retiring.Buffer);
        return true;
    });

    bool newUploads = false;
    bool surfaceLayoutChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        surfaceLayoutChanged = m_LatchedSurfaceSlotsPerRow != m_SurfaceSlotsPerRow;
        if (surfaceLayoutChanged)
        {
            m_LatchedSurfaceUploads.Clear();
            m_SurfaceStagingOffsets.Clear();
        }
        newUploads = !m_PublishedUploads.Empty() || !m_PublishedSurfaceUploads.Empty();
        for (auto& [slot, upload] : m_PublishedUploads)
            m_LatchedUploads.InsertOrAssign(slot, std::move(upload));
        m_PublishedUploads.Clear();
        for (auto& [slot, upload] : m_PublishedSurfaceUploads)
            m_LatchedSurfaceUploads.InsertOrAssign(slot, std::move(upload));
        m_PublishedSurfaceUploads.Clear();
        m_LatchedTables = m_PublishedTables; // one shared pointer per paged terrain and field
        m_LatchedChanges.insert(m_LatchedChanges.end(), m_PublishedChanges.begin(), m_PublishedChanges.end());
        m_PublishedChanges.clear();
        m_LatchedCacheDim = m_CacheDim;
        m_LatchedSurfaceSlotsPerRow = m_SurfaceSlotsPerRow;
    }
    if (surfaceLayoutChanged && m_Staging.IsValid())
    {
        m_Device->DestroyBuffer(m_Staging);
        m_Staging = {};
    }
    // The field changed its slots (a planet came or went): its tables are empty, so no slot of the
    // old textures is read. The device frees them after the frames in flight.
    for (PlaneCache& plane : m_Planes)
        if (plane.Texture.IsValid() && plane.SlotsPerRow != m_LatchedSurfaceSlotsPerRow)
        {
            DestroyValidTextures(device, std::span(&plane.Texture, 1));
            plane = PlaneCache{};
        }
    if (m_LatchedUploads.Empty() && m_LatchedSurfaceUploads.Empty())
    {
        m_CommittedTables = m_LatchedTables;
        CommitChanges();
        UpdateReadiness();
        return;
    }
    if (newUploads || !m_Staging.IsValid())
        StageUploads();
    UpdateReadiness();
}

void TerrainPageFeature::StageUploads()
{
    // Previous staging has not submitted; submitted buffers live in m_Retiring.
    if (m_Staging.IsValid())
        m_Device->DestroyBuffer(m_Staging);
    m_Staging = {};
    m_UploadRecorded = false;
    m_RecordedPlanes = {};
    EnsureCache();
    EnsurePlaneCaches();
    if (!m_LatchedUploads.Empty() && !m_Cache.IsValid())
        return;
    for (const auto& [slot, upload] : m_LatchedSurfaceUploads)
        for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
            if (((upload.PlaneMask >> plane) & 1u) != 0u && !m_Planes[plane].Texture.IsValid())
                return;
    std::size_t bytes = m_LatchedUploads.Size() * kPageBytes;
    m_SurfaceStagingOffsets.Clear();
    for (const auto& [slot, upload] : m_LatchedSurfaceUploads)
    {
        bytes = (bytes + kSurfaceStagingAlignment - 1u) / kSurfaceStagingAlignment * kSurfaceStagingAlignment;
        m_SurfaceStagingOffsets.InsertOrAssign(slot, bytes);
        bytes += upload.Bytes.size();
    }
    m_Staging = m_Device->CreateUploadBuffer(bytes, "Terrain.PageStaging");
    if (!m_Staging.IsValid())
    {
        Logger::Log::Warning("Terrain pages: no staging for {} height and {} surface page uploads ({} bytes); the page "
                             "tables keep their last uploaded state and the uploads retry next frame",
                             m_LatchedUploads.Size(), m_LatchedSurfaceUploads.Size(), bytes);
        return;
    }
    std::size_t offset = 0;
    for (const auto& [slot, upload] : m_LatchedUploads)
    {
        m_Device->UpdateBuffer(m_Staging, offset, kPageBytes, upload.Bytes.data());
        offset += kPageBytes;
    }
    for (const auto& [slot, upload] : m_LatchedSurfaceUploads)
        m_Device->UpdateBuffer(m_Staging, *m_SurfaceStagingOffsets.Find(slot), upload.Bytes.size(), upload.Bytes.data());
}

TerrainPageTableRing::Slot TerrainPageFeature::BindTableRing(uint32 terrain, uint32 generation,
                                                             const Rendering::RenderGraph::RGFrame* frame)
{
    const TableSet& tables = HasPlannedUploads(frame) ? m_LatchedTables : m_CommittedTables;
    const PackedPageTable* height = FindTable(tables.Height, terrain);
    if (m_Device == nullptr || height == nullptr)
        return {};
    const PackedPageTable* surface = FindTable(tables.Surface, terrain);
    Rendering::BufferHandle retired;
    const TerrainPageTableRing::Slot slot = m_TableRing.Bind(*m_Device, TerrainPageTableRing::Key{terrain, generation},
                                                             *height, surface, m_Device->GetFrameIndex(), retired);
    if (retired.IsValid())
        m_Retiring.push_back({retired, m_LatchedFrame});
    return slot;
}

void TerrainPageFeature::RequestTableReport()
{
    m_ReportRequested.store(true, std::memory_order_release);
}

bool TerrainPageFeature::LastTableReport(TableReport& out) const
{
    std::lock_guard<std::mutex> lock(m_ReportMutex);
    if (m_HasReport)
        out = m_LastReport;
    return m_HasReport;
}

void TerrainPageFeature::PublishTableHashes(const TerrainPages& pages, std::span<const uint32> paged)
{
    GE_CPU_PROFILE_SCOPE("Terrain.PageTable.ReportHashes");
    std::vector<PublishedTable> hashes;
    for (const uint32 terrain : paged)
        if (const PackedPageTable* table = pages.PackedTable(terrain))
            hashes.push_back(PublishedTable{terrain, table->Version(), static_cast<uint32>(table->Words().size()),
                                       pages.WholeTableHash(terrain)});
    std::lock_guard<std::mutex> lock(m_ReportMutex);
    m_PublishedTableHashes = std::move(hashes);
    m_TableHashesPublished = true;
}

void TerrainPageFeature::DeclareTableReport(Rendering::RenderGraph::RGFrame& frame)
{
    if (m_Device == nullptr)
        return;
    CompleteTableReport();
    if (m_ReportCopy.Step != ReportCopy::Stage::None)
        return;
    {
        std::lock_guard<std::mutex> lock(m_ReportMutex);
        if (!m_TableHashesPublished)
            return;
        m_ReportCopy.Tables = std::move(m_PublishedTableHashes);
        m_PublishedTableHashes.clear();
        m_TableHashesPublished = false;
    }
    m_ReportCopy.Step = ReportCopy::Stage::Declared;
    m_ReportCopy.FrameKey = &frame;
    m_ReportCopy.Frame = frame.FrameIndex();
    // The copy reads the slot that this frame binds; the slot's state is taken at the submit (SealSlotCopy).
    frame.AddPass(
        "Terrain.PageTableRing.Report", static_cast<int32_t>(Rendering::PassPhase::kFinalize),
        [](Rendering::RenderGraph::RGPassBuilder& p) { p.PreventCulling(); },
        [this](Rendering::RenderGraph::RGContext& ctx)
        {
            if (ctx.Cmd != nullptr)
                m_ReportCopy.Recorded = m_TableRing.RecordSlotCopy(*m_Device, *ctx.Cmd, m_Device->GetFrameIndex());
        });
}

void TerrainPageFeature::OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                                            const Rendering::IDevice::GpuSyncToken& token)
{
    if (m_ReportCopy.Step != ReportCopy::Stage::Declared || m_ReportCopy.FrameKey != &frame)
        return;
    // A frame begun again before its submit never recorded the copy.
    if (m_ReportCopy.Frame != frame.FrameIndex() || !m_ReportCopy.Recorded || !m_TableRing.SealSlotCopy())
    {
        m_ReportCopy = {};
        return;
    }
    m_ReportCopy.Token = token;
    m_ReportCopy.Step = ReportCopy::Stage::Sealed;
}

void TerrainPageFeature::CompleteTableReport()
{
    if (m_ReportCopy.Step != ReportCopy::Stage::Sealed ||
        m_Device->QueryGpuSyncToken(m_ReportCopy.Token) != Rendering::IDevice::GpuSyncStatus::Complete)
        return;
    GE_CPU_PROFILE_SCOPE("Terrain.PageTable.ReportRead");
    TableReport report;
    report.Frame = m_ReportCopy.Frame;
    report.Tables = std::move(m_ReportCopy.Tables);
    const bool read = m_TableRing.ReadSlotCopy(*m_Device, report.Slot);
    m_ReportCopy = {};
    if (!read)
        return;
    std::lock_guard<std::mutex> lock(m_ReportMutex);
    m_LastReport = std::move(report);
    m_HasReport = true;
}

void TerrainPageFeature::DeclareUploads(Rendering::RenderGraph::RGFrame& frame,
                                       Rendering::RenderGraph::RGPassBuilder& pass)
{
    if (!HasStagedUploads())
        return;
    m_UploadGraph = &frame;
    m_UploadFrame = frame.FrameIndex();
    m_UploadRecorded = false;
    m_RecordedPlanes = {};
    if (!m_LatchedUploads.Empty())
    {
        const auto cache = frame.ImportExternalTexture("Terrain.HeightPageCache", m_Cache,
            m_CacheCleared ? Rendering::ResourceState::ShaderResource : Rendering::ResourceState::Undefined);
        pass.Write(cache, Rendering::RenderGraph::RGTextureWrite::CopyDst);
        frame.MarkOutput(cache, Rendering::RenderGraph::RGImageLayout::ShaderReadOnly);
    }
    for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
    {
        const bool writesPlane = std::any_of(m_LatchedSurfaceUploads.begin(), m_LatchedSurfaceUploads.end(),
            [plane](const auto& entry) { return ((entry.second.PlaneMask >> plane) & 1u) != 0u; });
        if (!writesPlane)
            continue;
        const PlaneCache& cache = m_Planes[plane];
        const auto texture = frame.ImportExternalTexture(kPlaneCacheNames[plane], cache.Texture,
            cache.Written ? Rendering::ResourceState::ShaderResource : Rendering::ResourceState::Undefined);
        pass.Write(texture, Rendering::RenderGraph::RGTextureWrite::CopyDst);
        frame.MarkOutput(texture, Rendering::RenderGraph::RGImageLayout::ShaderReadOnly);
    }
    pass.Prepare([this, graph = &frame, frameIndex = m_UploadFrame, staging = m_Staging]
                 (Rendering::RenderGraph::RGPrepareContext&)
    {
        return m_UploadGraph == graph && m_UploadFrame == frameIndex && m_Staging == staging;
    });
    pass.OnSubmitted([this, graph = &frame, frameIndex = m_UploadFrame, staging = m_Staging]
    {
        if (m_UploadGraph == graph && m_UploadFrame == frameIndex && m_Staging == staging)
            SubmitUploads(*graph);
    });
}

void TerrainPageFeature::SubmitUploads(Rendering::RenderGraph::RGFrame& frame)
{
    if (!m_UploadRecorded || !m_Staging.IsValid())
        return;
    m_CommittedTables = m_LatchedTables;
    CommitChanges();
    if (!m_LatchedUploads.Empty())
        m_CacheCleared = true;
    for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
        m_Planes[plane].Written |= m_RecordedPlanes[plane];
    uint64 bytes = m_LatchedUploads.Size() * kPageBytes;
    for (const auto& [slot, upload] : m_LatchedSurfaceUploads)
        bytes += upload.Bytes.size();
    m_Retiring.push_back({m_Staging, m_LatchedFrame, frame.SubmissionToken(), &frame, bytes});
    m_Staging = {};
    m_LatchedUploads.Clear();
    m_LatchedSurfaceUploads.Clear();
    m_SurfaceStagingOffsets.Clear();
    m_UploadRecorded = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Readiness.SubmittedFrame = m_UploadFrame;
    }
    UpdateReadiness();
}

void TerrainPageFeature::CommitChanges()
{
    if (m_LatchedChanges.empty())
        return;
    ++m_ChangeSerial;
    for (const PageChangeRect& rect : m_LatchedChanges)
    {
        std::vector<uint64>& grid = m_ChangeGrids.GetOrInsert(rect.Terrain);
        grid.resize(static_cast<std::size_t>(kChangeGridCells) * kChangeGridCells, 0u);
        const GridCells cells = CellsOf(rect.UV, kChangeGridCells);
        for (uint32 z = cells.Z0; z <= cells.Z1; ++z)
            std::fill_n(grid.begin() + z * kChangeGridCells + cells.X0, cells.X1 - cells.X0 + 1u, m_ChangeSerial);
    }
    m_LatchedChanges.clear();
}

uint64 TerrainPageFeature::ChangeSerialOver(uint32 terrain, const Mathematics::Rect& uv) const
{
    const std::vector<uint64>* grid = m_ChangeGrids.Find(terrain);
    if (grid == nullptr || uv.Width < 0.0f || uv.Height < 0.0f)
        return 0u;
    const GridCells cells = CellsOf(uv, kChangeGridCells);
    uint64 latest = 0;
    for (uint32 z = cells.Z0; z <= cells.Z1; ++z)
        for (uint32 x = cells.X0; x <= cells.X1; ++x)
            latest = std::max(latest, (*grid)[z * kChangeGridCells + x]);
    return latest;
}

void TerrainPageFeature::EnsureCache()
{
    if (m_Cache.IsValid() || m_LatchedCacheDim == 0)
        return;
    m_Cache = CreateCacheTexture(m_LatchedCacheDim, Rendering::TextureFormat::R32_FLOAT, "Terrain.HeightPageCache");
    m_CacheCleared = false;
    m_CacheReadable = false;
}

void TerrainPageFeature::EnsurePlaneCaches()
{
    uint32 planes = 0;
    for (const auto& [slot, upload] : m_LatchedSurfaceUploads)
        planes |= upload.PlaneMask;
    const uint32 slotsPerRow = m_LatchedSurfaceSlotsPerRow;
    for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
    {
        if (m_Planes[plane].Texture.IsValid() || ((planes >> plane) & 1u) == 0u || slotsPerRow == 0)
            continue;
        const PageStreaming::PageFieldFormat format = SurfacePlaneFormat(kSurfacePlanes[plane]);
        const uint32 dim = slotsPerRow * PageStreaming::PageStrideTexels(format);
        m_Planes[plane] =
            PlaneCache{CreateCacheTexture(dim, PlaneTextureFormat(format), kPlaneCacheNames[plane]), slotsPerRow};
        if (!m_Planes[plane].Texture.IsValid())
            Logger::Log::Warning("Terrain pages: the {} surface page cache ({} x {}) was not created; the plane's "
                                 "pages do not upload",
                                 SurfacePlaneName(kSurfacePlanes[plane]), dim, dim);
    }
}

Rendering::TextureHandle TerrainPageFeature::CreateCacheTexture(uint32 dim, Rendering::TextureFormat format,
                                                                const char* name)
{
    Rendering::TextureDesc desc{};
    desc.width = dim;
    desc.height = dim;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                 static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    desc.persistent = true;
    desc.debugName = name;
    return m_Device->CreateTexture(desc);
}

void TerrainPageFeature::FlushSurfaceUploads(Rendering::CommandList& cl)
{
    const uint32 slotsPerRow = m_LatchedSurfaceSlotsPerRow;
    std::array<bool, kSurfacePlanes.size()> copied{};
    for (const auto& [slot, upload] : m_LatchedSurfaceUploads)
    {
        std::size_t offset = *m_SurfaceStagingOffsets.Find(slot);
        for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
        {
            if (((upload.PlaneMask >> plane) & 1u) == 0u)
                continue;
            const PageStreaming::PageFieldFormat format = SurfacePlaneFormat(kSurfacePlanes[plane]);
            const uint32 stride = PageStreaming::PageStrideTexels(format);
            const PlaneCache& cache = m_Planes[plane];
            if (cache.Texture.IsValid())
            {
                cl.CopyBufferToTextureSubresource(m_Staging, cache.Texture, 0, 0, stride, stride, offset, 0, 1, 0,
                                                  (slot % slotsPerRow) * stride,
                                                  (slot / slotsPerRow) * stride, Rendering::ResourceState::CopyDest);
                copied[plane] = true;
            }
            offset += PageStreaming::PagePayloadBytes(format);
        }
    }
    for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
    {
        if (!copied[plane])
            continue;
        m_RecordedPlanes[plane] = true;
    }
}

void TerrainPageFeature::FlushUploads(Rendering::CommandList& cl,
                                      const Rendering::RenderGraph::RGFrame& frame)
{
    if (!m_Staging.IsValid() || !HasPlannedUploads(&frame))
        return;
    FlushSurfaceUploads(cl);
    m_UploadRecorded = !m_LatchedSurfaceUploads.Empty();
    if (!m_Cache.IsValid() || m_LatchedUploads.Empty())
        return;
    if (!m_CacheCleared)
    {
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cl.ClearColorImageSubresource(m_Cache, 0, 0, zero);
    }

    const uint32 slotsPerRow = m_LatchedCacheDim / PageStreaming::kPageStrideSamples;
    std::size_t offset = 0;
    for (const auto& [slot, upload] : m_LatchedUploads)
    {
        cl.CopyBufferToTextureSubresource(m_Staging, m_Cache, 0, 0, PageStreaming::kPageStrideSamples,
                                          PageStreaming::kPageStrideSamples, offset,
                                          PageStreaming::kPageStrideSamples * sizeof(float32), 1, 0,
                                          (slot % slotsPerRow) * PageStreaming::kPageStrideSamples,
                                          (slot / slotsPerRow) * PageStreaming::kPageStrideSamples, Rendering::ResourceState::CopyDest);
        offset += kPageBytes;
    }
    m_UploadRecorded = true;
}

const PackedPageTable* TerrainPageFeature::FindTable(const TableMap& tables, uint32 terrain)
{
    const Table* table = tables.Find(terrain);
    if (table == nullptr || !table->Snapshot || table->Snapshot->Words().empty())
        return nullptr;
    return table->Snapshot.get();
}

bool TerrainPageFeature::HasPlannedUploads(const Rendering::RenderGraph::RGFrame* frame) const
{
    return frame != nullptr && frame == m_UploadGraph && frame->FrameIndex() == m_UploadFrame;
}


Rendering::TextureHandle TerrainPageFeature::CacheTexture(const Rendering::RenderGraph::RGFrame* frame) const
{
    return m_CacheReadable || HasPlannedUploads(frame) ? m_Cache : Rendering::TextureHandle{};
}

bool TerrainPageFeature::LatchedTable(uint32 terrain, std::span<const uint32>& words, uint64& version,
                                           uint64* contentGeneration,
                                           const Rendering::RenderGraph::RGFrame* frame, uint64* revision) const
{
    const Table* table = HasPlannedUploads(frame) ? m_LatchedTables.Height.Find(terrain) : m_CommittedTables.Height.Find(terrain);
    if (table == nullptr || !table->Snapshot || table->Snapshot->Words().empty())
        return false;
    words = table->Snapshot->Words();
    version = table->Version;
    if (contentGeneration != nullptr)
        *contentGeneration = table->ContentGeneration;
    if (revision != nullptr)
        *revision = table->Revision;
    return true;
}


bool TerrainPageFeature::DirtyRegionSince(uint32 terrain, uint32 terrainGeneration, uint64 consumedRevision,
                                               uint64 selectedRevision,
                                               const Rendering::RenderGraph::RGFrame* frame,
                                               HeightPageDirtyRect& region) const
{
    const Table* table = HasPlannedUploads(frame) ? m_LatchedTables.Height.Find(terrain) : m_CommittedTables.Height.Find(terrain);
    if (table == nullptr || !table->DirtyPending || table->DirtyFullRequired ||
        table->Revision != selectedRevision || consumedRevision == 0 ||
        consumedRevision < table->DirtyBaseRevision || consumedRevision >= selectedRevision ||
        table->Dirty.TerrainGeneration != terrainGeneration)
        return false;
    region = table->Dirty;
    return true;
}


void TerrainPageFeature::AcknowledgePublication(uint32 terrain, uint64 revision)
{
    if (revision == 0)
        return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    Table* table = m_PublishedTables.Height.Find(terrain);
    if (table != nullptr && table->Revision == revision)
    {
        table->DirtyBaseRevision = revision;
        table->DirtyPending = false;
        table->DirtyFullRequired = false;
        table->Dirty = {};
    }
}


bool TerrainPageFeature::HasPendingPublication(uint32 terrain, uint64 version, uint64 contentGeneration) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const Table* requested = m_PublishedTables.Height.Find(terrain);
    return requested != nullptr && (requested->Version != version || requested->ContentGeneration != contentGeneration);
}


bool TerrainPageFeature::LatchedSurfaceTable(uint32 terrain, std::span<const uint32>& words, uint64& version,
                                            const Rendering::RenderGraph::RGFrame* frame) const
{
    const TableSet& tables = HasPlannedUploads(frame) ? m_LatchedTables : m_CommittedTables;
    const PackedPageTable* table = FindTable(tables.Surface, terrain);
    if (table == nullptr)
        return false;
    words = table->Words();
    version = table->Version();
    return true;
}

Rendering::TextureHandle TerrainPageFeature::SurfacePlaneTexture(uint32 terrain,
                                                                 PageStreaming::PageFieldKind plane,
                                                                 const Rendering::RenderGraph::RGFrame* frame) const
{
    const bool planned = HasPlannedUploads(frame);
    const Table* table = (planned ? m_LatchedTables.Surface : m_CommittedTables.Surface).Find(terrain);
    if (table == nullptr)
        return {};
    for (std::size_t index = 0; index < kSurfacePlanes.size(); ++index)
    {
        if (kSurfacePlanes[index] != plane)
            continue;
        const bool terrainHasPlane = ((table->PlaneMask >> index) & 1u) != 0u;
        return terrainHasPlane && (m_Planes[index].Readable || planned) ? m_Planes[index].Texture : Rendering::TextureHandle{};
    }
    return {};
}

TerrainPageFeature::Readiness TerrainPageFeature::GetReadiness() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    Readiness snapshot = m_Readiness;
    snapshot.PublishedUploads = m_PublishedUploads.Size();
    snapshot.PublishedSurfaceUploads = m_PublishedSurfaceUploads.Size();
    snapshot.PendingBytes += snapshot.PublishedUploads * kPageBytes;
    for (const auto& [slot, upload] : m_PublishedSurfaceUploads)
        snapshot.PendingBytes += upload.Bytes.size();
    for (const auto& [terrain, requested] : m_PublishedTables.Height)
    {
        auto submitted = std::find_if(snapshot.Terrains.begin(), snapshot.Terrains.end(),
            [terrain](const TerrainReadiness& state) { return state.Terrain == terrain; });
        if (submitted == snapshot.Terrains.end())
            snapshot.Terrains.push_back({terrain, requested.Version, 0, requested.ContentGeneration, 0});
        else
        {
            submitted->RequestedVersion = requested.Version;
            submitted->RequestedContentGeneration = requested.ContentGeneration;
        }
    }
    return snapshot;
}

void TerrainPageFeature::UpdateReadiness()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Readiness.CacheBytes = m_Cache.IsValid() ? uint64{m_LatchedCacheDim} * m_LatchedCacheDim * sizeof(float32) : 0;
    for (std::size_t plane = 0; plane < kSurfacePlanes.size(); ++plane)
        if (m_Planes[plane].Texture.IsValid())
            m_Readiness.CacheBytes += uint64{m_LatchedSurfaceSlotsPerRow} * m_LatchedSurfaceSlotsPerRow *
                                      PageStreaming::PagePayloadBytes(SurfacePlaneFormat(kSurfacePlanes[plane]));
    m_Readiness.CacheReady = m_CacheCleared;
    m_Readiness.StagingReady = m_Staging.IsValid();
    m_Readiness.LatchedFrame = m_LatchedFrame;
    m_Readiness.LatchedUploads = m_LatchedUploads.Size();
    m_Readiness.LatchedSurfaceUploads = m_LatchedSurfaceUploads.Size();
    m_Readiness.PendingBytes = m_LatchedUploads.Size() * kPageBytes;
    for (const auto& [slot, upload] : m_LatchedSurfaceUploads)
        m_Readiness.PendingBytes += upload.Bytes.size();
    m_Readiness.RetiringBytes = 0;
    for (const RetiringBuffer& buffer : m_Retiring)
        m_Readiness.RetiringBytes += buffer.Bytes;
    m_Readiness.Terrains.clear();
    for (const auto& [terrain, table] : m_CommittedTables.Height)
        m_Readiness.Terrains.push_back({terrain, 0, table.Version, 0, table.ContentGeneration});
}

void TerrainPageFeature::OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame)
{
    if (m_ReportCopy.Step == ReportCopy::Stage::Declared && m_ReportCopy.FrameKey == &frame)
        m_ReportCopy = {};
    std::erase_if(m_Retiring, [this, &frame](const RetiringBuffer& buffer)
    {
        if (buffer.Stream != &frame)
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

void TerrainPageFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    // Every handle died with the old device: forget them without destroying them.
    m_Device = device;
    m_Cache = {};
    m_CacheCleared = false;
    m_CacheReadable = false;
    m_Staging = {};
    m_Retiring.clear();
    m_UploadGraph = nullptr;
    m_UploadRecorded = false;
    m_RecordedPlanes = {};
    m_TableRing.Release(nullptr);
    m_ReportCopy = {};
    m_Planes = {};
    m_LatchedUploads.Clear();
    m_LatchedSurfaceUploads.Clear();
    m_SurfaceStagingOffsets.Clear();
    m_LatchedTables = {};
    m_CommittedTables = {};
    m_LatchedChanges.clear();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PublishedUploads.Clear();
        m_PublishedSurfaceUploads.Clear();
        m_PublishedChanges.clear();
        for (auto& [terrain, table] : m_PublishedTables.Height)
        {
            table.DirtyBaseRevision = table.Revision;
            table.Revision = ++m_PublicationRevision;
            table.DirtyPending = true;
            table.DirtyFullRequired = true;
        }
        m_Readiness = {};
    }
    m_LatchedFrame = ~0ull;
    m_DeviceEpoch.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace GameEngine::TerrainECS
