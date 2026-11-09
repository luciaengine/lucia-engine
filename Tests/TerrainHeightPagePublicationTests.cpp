#include "TerrainECS/TerrainHeightPageFeature.h"
#include "TerrainPageGpuProbe.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainUploadNode.h"

#include "Engine/Rendering/RenderServices.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageTableEntry.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace GameEngine::TerrainECS::Test
{
namespace
{

PagedTerrainRequest PublicationRequest(uint64 identity)
{
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Noise;
    lattice.TileWorldSize = 256.0f;
    lattice.TileSamples = 257;
    lattice.TilesX = 1;
    lattice.TilesZ = 1;
    lattice.Noise = {0.01f, 1.0f, 2u, static_cast<uint32>(identity)};
    PagedTerrainRequest request;
    request.Terrain = 3;
    request.TerrainGeneration = 7;
    request.SourceIdentity = identity;
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.Level0TexelX = 1.0f;
    request.Shape.Level0TexelZ = 1.0f;
    request.Shape.SamplesX = 257;
    request.Shape.SamplesZ = 257;
    request.Shape.MaxHeight = 1.0f;
    return request;
}

void LoadPublication(TerrainHeightPages& pages, const PagedTerrainRequest& request, uint64 firstFrame)
{
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    for (uint64 frame = firstFrame; frame < firstFrame + 20 && pages.Uploads().empty(); ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
}

void LoadSettledPublication(TerrainHeightPages& pages, const PagedTerrainRequest& request)
{
    FlatMap<uint32, HeightPageUpload> payloads;
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    for (uint64 frame = 2; frame < 90; ++frame)
    {
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        for (auto& upload : pages.Uploads())
            payloads.InsertOrAssign(upload.Slot, std::move(upload));
        pages.Uploads().clear();
    }
    for (auto& [slot, upload] : payloads)
        pages.Uploads().push_back(std::move(upload));
}

class TerrainHeightPagePublication : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Rendering::DeviceDesc description{};
        description.preferredAPI = Rendering::GraphicsAPI::Vulkan;
        description.enableDebugLayer = true;
        description.enableDynamicRendering = true;
        m_Device = Rendering::DeviceFactory::CreateDevice(description);
        if (!m_Device || !m_Device->Initialize(description))
        {
            m_Device.reset();
            GTEST_SKIP() << "no Vulkan device";
        }
        if (!m_Device->GetValidationStats().Enabled)
            GTEST_SKIP() << "Vulkan validation layer unavailable";
        m_Services = std::make_unique<Engine::Renderer::RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
        m_Publication = &m_Services->EnsureFeature<TerrainHeightPageFeature>();
        m_Pages = std::make_unique<TerrainHeightPages>(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.DenseTableWords});
        m_Pages->Update(1, 1.0f / 60.0f, false, 2);
        ASSERT_TRUE(m_Upload.Initialize("Upload", "{}", nullptr));
        m_Persistent = std::make_unique<Rendering::RenderGraph::RGResourcePool>(m_Device.get());
        m_Transient = std::make_unique<Rendering::RenderGraph::RGTransientPool>(m_Device.get());
        m_Ring = std::make_unique<Rendering::RenderGraph::RGUploadRing>(m_Device.get(), 2, 4096);
        m_Frame = std::make_unique<Rendering::RenderGraph::RGFrame>(m_Device.get(), m_Persistent.get(), m_Transient.get(), m_Ring.get());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Publication && m_Frame)
            m_Publication->OnFrameStreamRetiredRG(*m_Frame);
        m_Frame.reset();
        m_Ring.reset();
        m_Transient.reset();
        m_Persistent.reset();
        m_Services.reset();
        if (m_Device->GetValidationStats().Enabled)
        {
            EXPECT_EQ(m_Device->GetValidationStats().ErrorCount, 0u);
        }
        m_Device->Shutdown();
    }

    void DeclareUpload()
    {
        Rendering::ViewDesc view{};
        Engine::Renderer::Pipeline::ViewDeclare declaration(*m_Frame, view, *m_Services, "Upload");
        m_Upload.DeclareForView(declaration);
    }

    void Execute()
    {
        m_Frame->Execute();
        m_Device->WaitForIdle();
    }

    void RewritePublication(const HeightPageUpload& payload, float minimum, float maximum)
    {
        m_Pages->Uploads().push_back(payload);
        m_Pages->DirtyRects().clear();
        m_Pages->DirtyRects().push_back({3, 7, minimum, minimum, maximum, maximum});
        m_Publication->Publish(*m_Pages);
    }

    std::unique_ptr<Rendering::IDevice> m_Device;
    std::unique_ptr<Engine::Renderer::RenderServices> m_Services;
    std::unique_ptr<TerrainHeightPages> m_Pages;
    TerrainHeightPageFeature* m_Publication = nullptr;
    TerrainUploadNode m_Upload;
    std::unique_ptr<Rendering::RenderGraph::RGResourcePool> m_Persistent;
    std::unique_ptr<Rendering::RenderGraph::RGTransientPool> m_Transient;
    std::unique_ptr<Rendering::RenderGraph::RGUploadRing> m_Ring;
    std::unique_ptr<Rendering::RenderGraph::RGFrame> m_Frame;
};

} // namespace

TEST_F(TerrainHeightPagePublication, AbandonedAndOlderSnapshotsKeepNewerDirtyCoverage)
{
    const PagedTerrainRequest request = PublicationRequest(11);
    LoadSettledPublication(*m_Pages, request);
    const auto payloads = m_Pages->Uploads();
    ASSERT_FALSE(payloads.empty());
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    DeclareUpload();
    Execute();
    std::span<const uint32> words;
    uint64 version = 0, content = 0, originalRevision = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, nullptr, &originalRevision));
    m_Publication->AcknowledgePublication(3, originalRevision);
    RewritePublication(payloads.front(), 0.1f, 0.2f);
    m_Frame->BeginFrame(2);
    DeclareUpload();
    uint64 firstRevision = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, m_Frame.get(), &firstRevision));
    HeightPageDirtyRect region;
    ASSERT_TRUE(m_Publication->DirtyRegionSince(3, 7, originalRevision, firstRevision, m_Frame.get(), region));
    EXPECT_FLOAT_EQ(region.MinU, 0.1f);
    EXPECT_FLOAT_EQ(region.MaxU, 0.2f);

    RewritePublication(payloads.front(), 0.8f, 0.9f);
    // P2 cannot mutate the table/bounds already selected for P1.
    ASSERT_TRUE(m_Publication->DirtyRegionSince(3, 7, originalRevision, firstRevision, m_Frame.get(), region));
    EXPECT_FLOAT_EQ(region.MaxU, 0.2f);
    m_Publication->AcknowledgePublication(3, firstRevision);
    m_Frame->BeginFrame(4); // P1 abandoned; its payload and P2 coalesce at the same slot.
    DeclareUpload();
    uint64 secondRevision = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, m_Frame.get(), &secondRevision));
    ASSERT_GT(secondRevision, firstRevision);
    ASSERT_TRUE(m_Publication->DirtyRegionSince(3, 7, originalRevision, secondRevision, m_Frame.get(), region));
    EXPECT_FLOAT_EQ(region.MinU, 0.1f);
    EXPECT_FLOAT_EQ(region.MaxU, 0.9f);
    EXPECT_FALSE(m_Publication->DirtyRegionSince(3, 8, originalRevision, secondRevision, m_Frame.get(), region));
    EXPECT_FALSE(m_Publication->DirtyRegionSince(3, 7, 0, secondRevision, m_Frame.get(), region));
    EXPECT_FALSE(m_Publication->DirtyRegionSince(3, 7, originalRevision, firstRevision, m_Frame.get(), region));
    Execute();
    m_Publication->AcknowledgePublication(3, secondRevision);
    RewritePublication(payloads.front(), 0.4f, 0.5f);
    m_Frame->BeginFrame(6);
    DeclareUpload();
    uint64 thirdRevision = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, m_Frame.get(), &thirdRevision));
    EXPECT_FALSE(m_Publication->DirtyRegionSince(3, 7, firstRevision, thirdRevision, m_Frame.get(), region))
        << "a lagging consumer has no retained coverage before the latest caught-up acknowledgement";
    ASSERT_TRUE(m_Publication->DirtyRegionSince(3, 7, secondRevision, thirdRevision, m_Frame.get(), region));
    EXPECT_FLOAT_EQ(region.MinU, 0.4f);
    EXPECT_FLOAT_EQ(region.MaxU, 0.5f);
    Execute();
}

TEST_F(TerrainHeightPagePublication, MissingBoundsAndStaleDeviceAcknowledgementRequireFullEvaluation)
{
    const PagedTerrainRequest request = PublicationRequest(11);
    LoadSettledPublication(*m_Pages, request);
    const auto payloads = m_Pages->Uploads();
    ASSERT_FALSE(payloads.empty());
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    DeclareUpload();
    Execute();
    std::span<const uint32> words;
    uint64 version = 0, content = 0, originalRevision = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, nullptr, &originalRevision));
    m_Publication->AcknowledgePublication(3, originalRevision);
    m_Pages->Uploads().push_back(payloads.front());
    m_Pages->DirtyRects().clear();
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(2);
    DeclareUpload();
    uint64 missingRevision = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, m_Frame.get(), &missingRevision));
    HeightPageDirtyRect region;
    EXPECT_FALSE(m_Publication->DirtyRegionSince(3, 7, originalRevision, missingRevision, m_Frame.get(), region));
    Execute();
    m_Publication->AcknowledgePublication(3, missingRevision);

    uint64 frameKey = 4;
    for (const HeightPageDirtyRect invalid : {
             HeightPageDirtyRect{3, 7, 0.1f, 0.1f, std::numeric_limits<float32>::quiet_NaN(), 0.2f},
             HeightPageDirtyRect{3, 7, -0.1f, 0.1f, 0.2f, 0.2f},
             HeightPageDirtyRect{3, 8, 0.1f, 0.1f, 0.2f, 0.2f}})
    {
        m_Pages->Uploads().push_back(payloads.front());
        m_Pages->DirtyRects() = {invalid};
        m_Publication->Publish(*m_Pages);
        m_Frame->BeginFrame(frameKey);
        frameKey += 2;
        DeclareUpload();
        uint64 invalidRevision = 0;
        ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, m_Frame.get(), &invalidRevision));
        EXPECT_FALSE(m_Publication->DirtyRegionSince(3, 7, missingRevision, invalidRevision, m_Frame.get(), region));
        Execute();
        m_Publication->AcknowledgePublication(3, invalidRevision);
        missingRevision = invalidRevision;
    }
    m_Publication->OnDeviceRebuilt(m_Device.get());
    m_Publication->AcknowledgePublication(3, missingRevision); // stale pre-rebuild callback
    m_Pages->Uploads().push_back(payloads.front());
    m_Pages->DirtyRects() = {{3, 7, 0.1f, 0.1f, 0.2f, 0.2f}};
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(frameKey);
    DeclareUpload();
    uint64 resetRevision = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, content, m_Frame.get(), &resetRevision));
    EXPECT_GT(resetRevision, missingRevision);
    EXPECT_FALSE(m_Publication->DirtyRegionSince(3, 7, missingRevision, resetRevision, m_Frame.get(), region));
    Execute();
}

TEST_F(TerrainHeightPagePublication, AbandonedUploadRetainsSubmittedTableAndRetriesItsPayload)
{
    namespace Graph = Rendering::RenderGraph;
    const PagedTerrainRequest original = PublicationRequest(11);
    LoadPublication(*m_Pages, original, 2);
    ASSERT_FALSE(m_Pages->Uploads().empty());
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    DeclareUpload();
    Execute();
    ASSERT_TRUE(m_Publication->CacheTexture().IsValid());
    std::span<const uint32> words;
    uint64 committedVersion = 0;
    uint64 contentGeneration = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(original.Terrain, words, committedVersion, contentGeneration));

    const PagedTerrainRequest replacement = PublicationRequest(29);
    LoadPublication(*m_Pages, replacement, 30);
    ASSERT_FALSE(m_Pages->Uploads().empty());
    uint64 requestedVersion = 0;
    ASSERT_NE(m_Pages->TableWords(replacement.Terrain, requestedVersion), nullptr);
    ASSERT_NE(requestedVersion, committedVersion);
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(2);
    DeclareUpload();
    m_Frame->AddPass("CancelPublication", 0,
        [](Graph::RGPassBuilder& pass)
        {
            pass.PreventCulling();
            pass.Prepare([](Graph::RGPrepareContext&) { return false; });
        }, [](Graph::RGContext&) {});
    Execute();
    ASSERT_EQ(m_Frame->Stats().PreparationFailures, 1u);
    ASSERT_EQ(m_Frame->Stats().SubmissionsMade, 0u);

    m_Publication->Latch(*m_Device, 3);
    uint64 visibleVersion = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(original.Terrain, words, visibleVersion, contentGeneration));
    EXPECT_EQ(visibleVersion, committedVersion) << "a declared copy was never submitted";
    EXPECT_TRUE(m_Publication->HasStagedUploads()) << "the abandoned payload must remain retryable";

    m_Frame->BeginFrame(4);
    DeclareUpload();
    Execute();
    ASSERT_TRUE(m_Publication->LatchedTable(replacement.Terrain, words, visibleVersion, contentGeneration));
    EXPECT_EQ(visibleVersion, requestedVersion);
    EXPECT_FALSE(m_Publication->HasStagedUploads());
}

TEST_F(TerrainHeightPagePublication, FirstCacheAndTableStayPlannedUntilTheirCopySubmits)
{
    LoadPublication(*m_Pages, PublicationRequest(11), 2);
    ASSERT_FALSE(m_Pages->Uploads().empty());
    for (auto& upload : m_Pages->Uploads())
        std::fill(upload.Samples.begin(), upload.Samples.end(), 0.5f);
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    DeclareUpload();
    std::span<const uint32> words;
    uint64 version = 0;
    uint64 generation = 0;
    EXPECT_FALSE(m_Publication->CacheTexture().IsValid());
    EXPECT_FALSE(m_Publication->LatchedTable(3, words, version, generation));
    EXPECT_TRUE(m_Publication->CacheTexture(m_Frame.get()).IsValid());
    EXPECT_TRUE(m_Publication->LatchedTable(3, words, version, generation, m_Frame.get()));
    Rendering::RenderGraph::RGFrame other(m_Device.get(), m_Persistent.get(), m_Transient.get(), m_Ring.get());
    other.BeginFrame(0);
    EXPECT_FALSE(m_Publication->CacheTexture(&other).IsValid());
    EXPECT_FALSE(m_Publication->LatchedTable(3, words, version, generation, &other));
    TerrainPageGpuProbe probe(*m_Device);
    ASSERT_TRUE(probe.Initialize());
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, generation, m_Frame.get()));
    const std::vector<TerrainPageGpuProbe::QueryPoint> points{{0.5f, 0.5f, 100.0f}};
    const auto results = probe.Declare(*m_Frame, m_Publication->CacheTexture(m_Frame.get()), words, points);
    Execute();
    EXPECT_FLOAT_EQ(probe.Read(results, 1).front(), 0.5f);
    EXPECT_TRUE(m_Publication->CacheTexture().IsValid());
    EXPECT_TRUE(m_Publication->LatchedTable(3, words, version, generation));
    EXPECT_FALSE(m_Publication->HasStagedUploads());
}

TEST_F(TerrainHeightPagePublication, InvalidatedPendingGraphCancelsInsteadOfSamplingUnsubmittedPages)
{
    LoadPublication(*m_Pages, PublicationRequest(11), 2);
    ASSERT_FALSE(m_Pages->Uploads().empty());
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    DeclareUpload();
    m_Publication->Latch(*m_Device, 1);
    Execute();
    EXPECT_EQ(m_Frame->Stats().PreparationFailures, 1u);
    EXPECT_EQ(m_Frame->Stats().SubmissionsMade, 0u);
    EXPECT_FALSE(m_Publication->CacheTexture().IsValid());
    EXPECT_TRUE(m_Publication->HasStagedUploads());
    m_Frame->BeginFrame(2);
    DeclareUpload();
    Execute();
    EXPECT_TRUE(m_Publication->CacheTexture().IsValid());
    EXPECT_FALSE(m_Publication->HasStagedUploads());
}

TEST_F(TerrainHeightPagePublication, ASubmittedPassWithoutRecordedCopiesRetainsItsPayload)
{
    namespace Graph = Rendering::RenderGraph;
    LoadPublication(*m_Pages, PublicationRequest(11), 2);
    ASSERT_FALSE(m_Pages->Uploads().empty());
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    m_Publication->Latch(*m_Device, 0);
    m_Frame->AddPass("UnrecordedUpload", 0,
        [this](Graph::RGPassBuilder& pass)
        {
            pass.PreventCulling();
            m_Publication->DeclareUploads(*m_Frame, pass);
        }, [](Graph::RGContext&) {});
    Execute();
    EXPECT_GT(m_Frame->Stats().SubmissionsMade, 0u);
    EXPECT_FALSE(m_Publication->CacheTexture().IsValid());
    EXPECT_TRUE(m_Publication->HasStagedUploads());
    m_Frame->BeginFrame(1);
    DeclareUpload();
    Execute();
    EXPECT_TRUE(m_Publication->CacheTexture().IsValid());
}

TEST_F(TerrainHeightPagePublication, DelayedRewritesCoalesceByPhysicalSlot)
{
    LoadPublication(*m_Pages, PublicationRequest(11), 2);
    ASSERT_FALSE(m_Pages->Uploads().empty());
    const auto payloads = m_Pages->Uploads();
    for (uint32 update = 0; update < 64; ++update)
    {
        m_Pages->Uploads() = payloads;
        for (auto& upload : m_Pages->Uploads())
            std::fill(upload.Samples.begin(), upload.Samples.end(), static_cast<float>(update + 1) / 128.0f);
        m_Publication->Publish(*m_Pages);
        const auto readiness = m_Publication->GetReadiness();
        EXPECT_EQ(readiness.PublishedUploads, payloads.size());
        EXPECT_LE(readiness.PublishedUploads, m_Pages->SlotCount());
    }
    m_Frame->BeginFrame(0);
    DeclareUpload();
    const auto readiness = m_Publication->GetReadiness();
    EXPECT_EQ(readiness.LatchedUploads, payloads.size());
    EXPECT_EQ(readiness.PublishedUploads, 0u);
    EXPECT_EQ(readiness.RetiringBytes, 0u);
    TerrainPageGpuProbe probe(*m_Device);
    ASSERT_TRUE(probe.Initialize());
    std::span<const uint32> words;
    uint64 version = 0;
    uint64 generation = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, version, generation, m_Frame.get()));
    const std::vector<TerrainPageGpuProbe::QueryPoint> points{{0.5f, 0.5f, 100.0f}};
    const auto results = probe.Declare(*m_Frame, m_Publication->CacheTexture(m_Frame.get()), words, points);
    Execute();
    EXPECT_FLOAT_EQ(probe.Read(results, 1).front(), 0.5f);
    EXPECT_EQ(m_Publication->GetReadiness().PendingBytes, 0u);
}

TEST_F(TerrainHeightPagePublication, StreamRetirementDrainsItsStagingBeforeTheTokenOwnerDies)
{
    LoadPublication(*m_Pages, PublicationRequest(11), 2);
    ASSERT_FALSE(m_Pages->Uploads().empty());
    const auto payloads = m_Pages->Uploads();
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    DeclareUpload();
    m_Frame->Execute();
    ASSERT_GT(m_Publication->GetReadiness().RetiringBytes, 0u);
    m_Publication->OnFrameStreamRetiredRG(*m_Frame);
    EXPECT_EQ(m_Publication->GetReadiness().RetiringBytes, 0u);
    m_Frame.reset();
    m_Frame = std::make_unique<Rendering::RenderGraph::RGFrame>(m_Device.get(), m_Persistent.get(), m_Transient.get(), m_Ring.get());
    m_Pages->Uploads() = payloads;
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(1);
    DeclareUpload();
    Execute();
    EXPECT_EQ(m_Publication->GetReadiness().PendingBytes, 0u);
}

TEST_F(TerrainHeightPagePublication, ReadersAroundASameSlotRewriteSeeTheirOwnPayloads)
{
    LoadSettledPublication(*m_Pages, PublicationRequest(11));
    ASSERT_FALSE(m_Pages->Uploads().empty());
    for (auto& upload : m_Pages->Uploads())
        std::fill(upload.Samples.begin(), upload.Samples.end(), 0.25f);
    const std::vector<HeightPageUpload> original = m_Pages->Uploads();
    m_Publication->Publish(*m_Pages);
    m_Frame->BeginFrame(0);
    DeclareUpload();
    Execute();
    std::span<const uint32> words;
    uint64 oldVersion = 0;
    uint64 oldGeneration = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, oldVersion, oldGeneration));
    const std::vector<uint32> oldWords(words.begin(), words.end());
    const uint32 topLevel = oldWords[2] - 1u;
    const uint32 rootEntry = oldWords[CBTTerrain::kCBTPageFadeOffsetWords + oldWords[4] +
                                    oldWords[CBTTerrain::kCBTPageHeaderWords + 4u * topLevel]];
    const uint32 rewrittenSlot = PageStreaming::PageEntrySlot(rootEntry);
    const auto rewritten = std::find_if(original.begin(), original.end(), [rewrittenSlot](const HeightPageUpload& upload)
    { return upload.Slot == rewrittenSlot; });
    ASSERT_NE(rewritten, original.end());
    m_Pages->Uploads() = {*rewritten};
    std::fill(m_Pages->Uploads().front().Samples.begin(), m_Pages->Uploads().front().Samples.end(), 0.75f);
    m_Publication->Publish(*m_Pages);
    EXPECT_TRUE(m_Publication->HasPendingPublication(3, oldVersion, oldGeneration));
    TerrainPageGpuProbe probe(*m_Device);
    ASSERT_TRUE(probe.Initialize());
    const std::vector<TerrainPageGpuProbe::QueryPoint> points{{0.25f, 0.25f, 100.0f}, {0.75f, 0.75f, 100.0f}};
    m_Frame->BeginFrame(1);
    m_Publication->Latch(*m_Device, 1);
    const auto before = probe.Declare(*m_Frame, m_Publication->CacheTexture(), oldWords, points);
    DeclareUpload();
    uint64 newVersion = 0;
    uint64 newGeneration = 0;
    ASSERT_TRUE(m_Publication->LatchedTable(3, words, newVersion, newGeneration, m_Frame.get()));
    EXPECT_EQ(newVersion, oldVersion) << "the rewrite changes payload without changing table words";
    EXPECT_GT(newGeneration, oldGeneration);
    const auto after = probe.Declare(*m_Frame, m_Publication->CacheTexture(m_Frame.get()), words, points);
    const auto untouched = std::find_if(original.begin(), original.end(), [rewrittenSlot](const HeightPageUpload& upload)
    { return upload.Slot != rewrittenSlot; });
    ASSERT_NE(untouched, original.end()) << "the partial-write witness needs a second populated slot";
    PageTable untouchedTable;
    untouchedTable.Reset(1, 1, m_Pages->SlotCount());
    untouchedTable.Entries[0] = PageStreaming::PackPageEntry(untouched->Slot, 0);
    untouchedTable.SlotFade[untouched->Slot] = 1.0f;
    std::vector<uint32> untouchedWords;
    PackPageTableWords(untouchedTable, m_Pages->Geometry(), untouchedWords);
    const auto preserved = probe.Declare(*m_Frame, m_Publication->CacheTexture(m_Frame.get()), untouchedWords, points);
    Execute();
    const auto oldHeights = probe.Read(before, points.size());
    const auto newHeights = probe.Read(after, points.size());
    const auto preservedHeights = probe.Read(preserved, points.size());
    for (std::size_t point = 0; point < points.size(); ++point)
    {
        EXPECT_FLOAT_EQ(oldHeights[point], 0.25f);
        EXPECT_FLOAT_EQ(newHeights[point], 0.75f);
        EXPECT_FLOAT_EQ(preservedHeights[point], 0.25f);
    }
    EXPECT_FALSE(m_Publication->HasPendingPublication(3, newVersion, newGeneration));
    const auto readiness = m_Publication->GetReadiness();
    EXPECT_EQ(readiness.PendingBytes, 0u);
    EXPECT_TRUE(readiness.CacheReady);
    ASSERT_EQ(readiness.Terrains.size(), 1u);
    EXPECT_EQ(readiness.Terrains.front().CommittedContentGeneration, newGeneration);
}

} // namespace GameEngine::TerrainECS::Test
