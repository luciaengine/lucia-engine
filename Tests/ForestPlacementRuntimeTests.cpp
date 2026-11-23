#include "PackedPlacementTestArchive.h"

#include "Assets/AssetManager.h"
#include "Engine/Rendering/Forest/ForestPlacementGpuUploads.h"
#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Engine/Rendering/Forest/ForestPlacementStream.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

class ForestPlacementRuntimeTest : public PackedPlacementArchiveTest
{
};

TEST_F(ForestPlacementRuntimeTest, KeepsFallbackThroughUploadAndProtectsRetiringSlotUntilLastDraw)
{
    using Engine::Renderer::ForestPlacementRuntime;
    using Engine::Renderer::ForestPlacementFrame;
    using Engine::Renderer::ForestPlacementView;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{20, 26, 35};
    std::string error;
    const auto planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    ForestPlacementView both{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    EXPECT_TRUE(planner->CanRequestDetails(both));
    auto overview = both;
    overview.Eye.Y += 1000;
    EXPECT_FALSE(planner->CanRequestDetails(overview));
    ForestPlacementFrame frame;
    ASSERT_TRUE(planner->Prepare(both, 1, 0, frame));
    EXPECT_TRUE(frame.WantsDetails);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 20u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 15u);
    EXPECT_EQ(frame.VisibleCells, 2u);
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto first = frame.Reads.front();
    EXPECT_EQ(first.Page, 0u);
    EXPECT_TRUE(planner->CompleteRead(first, kFirstPage));

    ASSERT_TRUE(planner->Prepare(both, 2, 0, frame));
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 15u);
    ASSERT_TRUE(planner->CompleteUpload(first, true));
    ASSERT_TRUE(planner->Prepare(both, 3, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, first);
    EXPECT_EQ(frame.DetailedDraws.front().PlacementCount, 1u);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 26u);

    ForestPlacementView secondOnly = both;
    secondOnly.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(planner->Prepare(secondOnly, 4, 2, frame));
    EXPECT_EQ(frame.VisibleCells, 1u);
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_TRUE(planner->Prepare(secondOnly, 5, 3, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto second = frame.Reads.front();
    EXPECT_EQ(second.Page, 1u);
    EXPECT_EQ(second.Slot, first.Slot);
    EXPECT_NE(second.Generation, first.Generation);
    EXPECT_FALSE(planner->CompleteUpload(first, true));

    // An invalid frame cannot lose the pending read or send it to the slot twice.
    EXPECT_FALSE(planner->Prepare(secondOnly, 5, 3, frame));
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(planner->CompleteRead(second, kSecondPage));
    EXPECT_TRUE(planner->CompleteUpload(second, true));
    ASSERT_TRUE(planner->Prepare(secondOnly, 6, 4, frame));
    EXPECT_TRUE(frame.StandDraws.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().PlacementCount, 2u);

    planner->ResetResidency();
    ASSERT_TRUE(planner->Prepare(secondOnly, 1, 0, frame));
    EXPECT_TRUE(frame.WantsDetails);
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    ASSERT_EQ(frame.Reads.size(), 1u);
    EXPECT_EQ(frame.Reads.front().Page, second.Page);
    EXPECT_NE(frame.Reads.front().Generation, second.Generation);
    EXPECT_FALSE(planner->CompleteRead(second, kSecondPage));
}

TEST_F(ForestPlacementRuntimeTest, ReturningViewReusesAReadyPageAndExtendsItsReaderFence)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "camera-return.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1, true);
    ASSERT_TRUE(runtime) << error;

    const ForestPlacementView near{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    auto turned = near;
    turned.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(near, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, true));
    ASSERT_TRUE(runtime->Prepare(near, 2, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);

    ASSERT_TRUE(runtime->Prepare(turned, 3, 1, frame));
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_TRUE(runtime->Prepare(near, 4, 1, frame));
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, ticket);
    EXPECT_EQ(runtime->ReadyPlacements(ticket).size(), 1u);

    auto overview = near;
    overview.DetailDistance = 0.0f;
    for (uint64 index = 5; index <= 9; ++index)
    {
        ASSERT_TRUE(runtime->Prepare(overview, index, index - 1, frame));
        EXPECT_TRUE(frame.DetailedDraws.empty());
        EXPECT_TRUE(frame.Reads.empty());
        ASSERT_EQ(frame.StandDraws.size(), 1u);
        EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);
    }
    // The old reader fence completed while the page was offscreen.
    ASSERT_TRUE(runtime->Prepare(near, 10, 9, frame));
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, ticket);

    ASSERT_TRUE(runtime->Prepare(turned, 11, 9, frame));
    EXPECT_TRUE(frame.DetailedDraws.empty());
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_TRUE(runtime->Prepare(turned, 12, 10, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    EXPECT_EQ(frame.Reads.front().Slot, ticket.Slot);
    EXPECT_NE(frame.Reads.front().Generation, ticket.Generation);
    EXPECT_EQ(frame.Reads.front().Page, 1u);
    EXPECT_TRUE(runtime->ReadyPlacements(ticket).empty());
}

TEST_F(ForestPlacementRuntimeTest, AllowsStandOnlyAndRejectsInvalidIndexOffsets)
{
    using Engine::Renderer::ForestPlacementRuntime;
    using Engine::Renderer::ForestPlacementFrame;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    constexpr std::array<uint32, 3> invalid{20, 19, 30};
    EXPECT_FALSE(ForestPlacementRuntime::Open(file, invalid, 0, 4, error));
    EXPECT_FALSE(error.empty());

    constexpr std::array<uint32, 3> offsets{20, 26, 35};
    const auto planner = ForestPlacementRuntime::Open(file, offsets, 0, 4, error);
    ASSERT_TRUE(planner) << error;
    ForestPlacementFrame frame;
    ASSERT_TRUE(planner->Prepare({{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f}, 1, 0, frame));
    EXPECT_FALSE(frame.WantsDetails);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 20u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 15u);
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    EXPECT_FALSE(planner->CompleteRead({0, 0, 0}, {}));
    EXPECT_FALSE(planner->CompleteUpload({0, 0, 0}, true));
}

TEST_F(ForestPlacementRuntimeTest, ColdDetailDemandRequiresAVisiblePageWithinLookahead)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "cold.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(runtime) << error;

    ForestPlacementView view{{512'000'032.0, 8.0, 512'000'001.0}, {}, 10.0f};
    // The root covers the middle of the gap. Neither page is within range.
    EXPECT_FALSE(runtime->CanRequestDetails(view));
    EXPECT_FALSE(runtime->CanPrefetchDetails(view));
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, plan));
    EXPECT_FALSE(plan.WantsDetails);
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    view.DetailDistance = 11.0f;
    EXPECT_TRUE(runtime->CanPrefetchDetails(view));
    EXPECT_FALSE(runtime->CanRequestDetails(view));
    view.EyeRelativePlanes[0] = {1, 0, 0, -15};
    EXPECT_FALSE(runtime->CanPrefetchDetails(view));
    view.EyeRelativePlanes = {};
    view.DetailDistance = 25.0f;
    EXPECT_TRUE(runtime->CanRequestDetails(view));
    view.EyeRelativePlanes[0] = {1, 0, 0, -15};
    EXPECT_TRUE(runtime->CanRequestDetails(view)); // Only the second page remains visible.
    view.DetailDistance = 15.0f;
    EXPECT_FALSE(runtime->CanRequestDetails(view)); // The near page is outside the frustum.
    view.EyeRelativePlanes[0] = {1, 0, 0, -100};
    view.DetailDistance = 100.0f;
    EXPECT_FALSE(runtime->CanRequestDetails(view));
}

TEST_F(ForestPlacementRuntimeTest, PrefetchKeepsTheStandUntilThePageEntersTheDetailRange)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "detail-lookahead.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1);
    ASSERT_TRUE(runtime) << error;

    ForestPlacementView view{{512'000'000.0 - 34.0, 8.0, 512'000'001.0}, {}, 25.0f};
    ASSERT_FALSE(runtime->CanRequestDetails(view));
    ASSERT_TRUE(runtime->CanPrefetchDetails(view));
    auto wideView = view;
    wideView.DetailDistance = std::numeric_limits<float>::max();
    EXPECT_TRUE(runtime->CanPrefetchDetails(wideView));
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, true));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);

    view.Eye.X = 512'000'000.0;
    ASSERT_TRUE(runtime->Prepare(view, 3, 2, frame));
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, ticket);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().FirstIndex, 6u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 6u);
}

TEST_F(ForestPlacementRuntimeTest, TransitionSlotStartsDemandWhilePreviousDetailDrawIsInFlight)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "transition.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1, false, 1);
    ASSERT_TRUE(runtime) << error;
    ASSERT_EQ(runtime->SlotCount(), 2u);

    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 25.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto first = frame.Reads.front();
    ASSERT_EQ(first.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(first, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(first, true));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, first);

    view.Eye.X = 512'000'104.0;
    ASSERT_TRUE(runtime->CanPrefetchDetails(view));
    ASSERT_FALSE(runtime->CanRequestDetails(view));
    ASSERT_TRUE(runtime->Prepare(view, 3, 1, frame));
    EXPECT_TRUE(frame.Reads.empty()); // This request cannot occupy the transition slot.
    EXPECT_TRUE(frame.DetailedDraws.empty());

    view.Eye.X = 512'000'055.0;
    ASSERT_TRUE(runtime->Prepare(view, 4, 1, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto second = frame.Reads.front();
    EXPECT_EQ(second.Page, 1u);
    EXPECT_NE(second.Slot, first.Slot);
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);
    ASSERT_TRUE(runtime->CompleteRead(second, kSecondPage));
    ASSERT_TRUE(runtime->CompleteUpload(second, true));

    ASSERT_TRUE(runtime->Prepare(view, 5, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, second);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 6u);
}

TEST_F(ForestPlacementRuntimeTest, ACameraCutReusesAReadingSlotBeforeItsIoCallback)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "cut.geforest";
    WriteStore(file);
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1);
    ASSERT_TRUE(planner) << error;

    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame plan;
    ASSERT_TRUE(planner->Prepare(view, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto oldRead = plan.Reads.front();
    ASSERT_EQ(oldRead.Page, 0u);

    view.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(planner->Prepare(view, 2, 0, plan));
    ASSERT_EQ(plan.CancelReads.size(), 1u);
    EXPECT_EQ(plan.CancelReads.front(), oldRead);
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto newRead = plan.Reads.front();
    EXPECT_EQ(newRead.Page, 1u);
    EXPECT_EQ(newRead.Slot, oldRead.Slot);
    EXPECT_NE(newRead.Generation, oldRead.Generation);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);

    EXPECT_FALSE(planner->CompleteRead(oldRead, kFirstPage));
    EXPECT_TRUE(planner->CompleteRead(newRead, kSecondPage));
    EXPECT_TRUE(planner->CompleteUpload(newRead, true));
    ASSERT_TRUE(planner->Prepare(view, 3, 2, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, newRead);
    EXPECT_TRUE(plan.StandDraws.empty());
}

TEST_F(ForestPlacementRuntimeTest, StreamRejectsArrivedBytesWhenTheCameraRetiresThePage)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{0, 6, 12};
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};

    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto first = plan.Reads.front();
    ASSERT_EQ(first.Page, 0u);
    ASSERT_TRUE(uploads.empty()); // In inline IO, its bytes already arrived.
    view.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    EXPECT_TRUE(uploads.empty());
    ASSERT_EQ(plan.CancelReads.size(), 1u);
    EXPECT_EQ(plan.CancelReads.front(), first);
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().FirstIndex, 6u);
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto second = plan.Reads.front();
    EXPECT_EQ(second.Page, 1u);
    EXPECT_EQ(second.Slot, first.Slot);
    EXPECT_NE(second.Generation, first.Generation);
    ASSERT_TRUE(stream.Prepare(assets, view, 3, 1, plan, uploads));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads.front().Ticket, second);
    const auto& bytes = uploads.front().Bytes;
    EXPECT_TRUE(std::ranges::equal(bytes, kSecondPage));
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_TRUE(planner->CompleteUpload(second, true));
    ASSERT_TRUE(stream.Prepare(assets, view, 4, 2, plan, uploads));
    EXPECT_TRUE(uploads.empty());
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, second);
    EXPECT_TRUE(plan.StandDraws.empty());
}

TEST_F(ForestPlacementRuntimeTest, AdjacentPagesShareAReadWhileKeepingIndependentUploadTickets)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "adjacent.geforest";
    WriteStore(file);
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    const ForestPlacementView view{{512'000'055.0, 8.0, 512'000'002.0}, {}, 100.0f};

    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 2u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    ASSERT_EQ(uploads.size(), 2u);
    EXPECT_EQ(uploads[0].Ticket.Page, 0u);
    EXPECT_EQ(uploads[1].Ticket.Page, 1u);
    EXPECT_EQ(uploads[0].Bytes.data() + uploads[0].Bytes.size(), uploads[1].Bytes.data());
    EXPECT_TRUE(std::ranges::equal(uploads[0].Bytes, kFirstPage));
    EXPECT_TRUE(std::ranges::equal(uploads[1].Bytes, kSecondPage));
}

TEST_F(ForestPlacementRuntimeTest, ACoalescedReadKeepsTheOtherPageWhenOneTicketIsRetired)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "partial.geforest";
    WriteStore(file);
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};

    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 2u);
    const auto retired = plan.Reads.front();
    ASSERT_EQ(retired.Page, 0u);
    view.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    ASSERT_EQ(plan.CancelReads.size(), 1u);
    EXPECT_EQ(plan.CancelReads.front(), retired);
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads.front().Ticket.Page, 1u);
    EXPECT_TRUE(std::ranges::equal(uploads.front().Bytes, kSecondPage));
    EXPECT_FALSE(planner->CanRecordUpload(retired));
}

TEST_F(ForestPlacementRuntimeTest, ACoalescedReadVerifiesEveryPageChecksumIndependently)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "corrupt-page.geforest";
    WriteStore(file);
    {
        std::fstream bytes(file, std::ios::in | std::ios::out | std::ios::binary);
        bytes.seekp(kDataOffset);
        bytes.put(0x13);
        ASSERT_TRUE(bytes);
    }
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 2u);
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    EXPECT_TRUE(plan.Reads.empty()); // The corrupt page keeps its retry backoff.
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads.front().Ticket.Page, 1u);
    EXPECT_TRUE(std::ranges::equal(uploads.front().Bytes, kSecondPage));
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
}

TEST_F(ForestPlacementRuntimeTest, StreamRetainsTheStandWhenTheAssetLoaderIsUnavailable)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{0, 6, 12};
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    AssetManager unavailable;
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ASSERT_TRUE(stream.Prepare(unavailable, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_TRUE(uploads.empty());
    ASSERT_TRUE(stream.Prepare(unavailable, view, 2, 1, plan, uploads));
    EXPECT_TRUE(uploads.empty());
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    ASSERT_TRUE(stream.Prepare(unavailable, view, 3, 2, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u); // The failed page is in cooldown.
}

TEST_F(ForestPlacementRuntimeTest, CombinesOnlyByteAdjacentForestCopiesAndPreservesTheStagingOrder)
{
    using namespace Engine::Renderer;
    constexpr size_t slotBytes = PackedPlacementStore::kPlacementsPerPage * sizeof(PackedPlacement);
    const std::array<uint8, slotBytes> full{};
    std::array<ForestPlacementUpload, 5> uploads{{
        {{2, 17, 1}, full}, {{5, 3, 1}, kSecondPage}, {{0, 5, 9}, kFirstPage},
        {{4, 1, 7}, kFirstPage}, {{3, 19, 3}, full}
    }};
    const auto plan = BuildForestPlacementUploadPlan(uploads, 6);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->SlotOrder.size(), uploads.size());
    EXPECT_EQ(plan->SlotOrder[0], &uploads[2]);
    EXPECT_EQ(plan->SlotOrder[1], &uploads[0]);
    EXPECT_EQ(plan->SlotOrder[2], &uploads[4]);
    EXPECT_EQ(plan->SlotOrder[3], &uploads[3]);
    EXPECT_EQ(plan->SlotOrder[4], &uploads[1]);
    EXPECT_EQ(plan->Bytes, 2 * slotBytes + 2 * kFirstPage.size() + kSecondPage.size());

    ASSERT_EQ(plan->Runs.size(), 3u);
    EXPECT_EQ(plan->Runs[0].StagingOffset, 0u);
    EXPECT_EQ(plan->Runs[0].SlotOffset, 0u);
    EXPECT_EQ(plan->Runs[0].Bytes, kFirstPage.size());
    EXPECT_EQ(plan->Runs[1].StagingOffset, kFirstPage.size());
    EXPECT_EQ(plan->Runs[1].SlotOffset, 2 * slotBytes);
    EXPECT_EQ(plan->Runs[1].Bytes, 2 * slotBytes + kFirstPage.size());
    EXPECT_EQ(plan->Runs[2].StagingOffset, 2 * slotBytes + 2 * kFirstPage.size());
    EXPECT_EQ(plan->Runs[2].SlotOffset, 5 * slotBytes);
    EXPECT_EQ(plan->Runs[2].Bytes, kSecondPage.size());

    uploads.back().Ticket.Slot = 2u;
    EXPECT_FALSE(BuildForestPlacementUploadPlan(uploads, 6));
    uploads.back().Ticket.Slot = 6u;
    EXPECT_FALSE(BuildForestPlacementUploadPlan(uploads, 6));
}

TEST_F(ForestPlacementRuntimeTest, FailedGraphUploadReleasesTheReservationAndPreservesCover)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{0, 6, 12};
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    ForestPlacementFrame plan;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ASSERT_TRUE(planner->Prepare(view, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    EXPECT_FALSE(planner->CanRecordUpload(ticket));
    ASSERT_TRUE(planner->CompleteRead(ticket, kFirstPage));
    EXPECT_TRUE(planner->CanRecordUpload(ticket));
    ForestPlacementUpload upload{ticket, kFirstPage};
    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    ForestPlacementGpuUploads uploader(planner, {}, 1);
    EXPECT_FALSE(uploader.Record(graph, std::span(&upload, 1)));
    EXPECT_FALSE(planner->CanRecordUpload(ticket));
    EXPECT_FALSE(planner->CompleteUpload(ticket, true));
    ASSERT_TRUE(planner->Prepare(view, 2, 1, plan));
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
    EXPECT_EQ(plan.Reads.front().Slot, ticket.Slot);
    EXPECT_NE(plan.Reads.front().Generation, ticket.Generation);
}

TEST_F(ForestPlacementRuntimeTest, StationaryDemandStillRetriesAndKeepsOtherViewScratchSeparate)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1);
    ASSERT_TRUE(runtime) << error;

    const ForestPlacementView near{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    auto otherView = near;
    otherView.EyeRelativePlanes[0] = {1, 0, 0, -35};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(near, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    EXPECT_FALSE(runtime->CompleteRead(plan.Reads.front(), {}));

    std::vector<DrawSubCommand> stand;
    runtime->GatherStand(otherView, stand);
    ASSERT_EQ(stand.size(), 1u);
    EXPECT_EQ(stand.front().FirstIndex, 6u);
    ASSERT_TRUE(runtime->Prepare(near, 2, 1, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
    const auto page = plan.Reads.front();
    ASSERT_TRUE(runtime->CompleteRead(page, kSecondPage));
    ASSERT_TRUE(runtime->CompleteUpload(page, true));

    for (uint64 frame = 3; frame < 31; ++frame)
    {
        runtime->GatherStand(otherView, stand);
        ASSERT_TRUE(runtime->Prepare(near, frame, frame - 1, plan));
        ASSERT_EQ(plan.DetailedDraws.size(), 1u);
        EXPECT_EQ(plan.DetailedDraws.front().Ticket, page);
        EXPECT_EQ(plan.VisibleCells, 2u);
        EXPECT_TRUE(plan.Reads.empty());
    }
    ASSERT_TRUE(runtime->Prepare(near, 31, 30, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    EXPECT_EQ(plan.VisibleCells, 2u);
}

} // namespace
} // namespace GameEngine::PageStreaming
