#include "PackedPlacementTestArchive.h"

#include "Assets/AssetManager.h"
#include "Engine/Rendering/Forest/ForestPlacementGpuUploads.h"
#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Engine/Rendering/Forest/ForestPlacementStream.h"
#include "ForestPlacementUploadPlan.h"
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

// True when a visible page is within the detail distance itself, not only the prefetch band.
bool WithinDetailRange(const Engine::Renderer::ForestPlacementRuntime& runtime,
                       Engine::Renderer::ForestPlacementView view)
{
    constexpr float kPrefetchDistanceScale = 1.4f;
    view.DetailDistance /= kPrefetchDistanceScale;
    return runtime.CanPrefetchDetails(view);
}

TEST_F(ForestPlacementRuntimeTest, KeepsFallbackThroughUploadAndProtectsRetiringSlotUntilLastDraw)
{
    using Engine::Renderer::ForestPlacementRuntime;
    using Engine::Renderer::ForestPlacementFrame;
    using Engine::Renderer::ForestPlacementView;
    using Engine::Renderer::ForestUploadResult;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    const auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    ForestPlacementView both{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    EXPECT_TRUE(WithinDetailRange(*runtime, both));
    auto overview = both;
    overview.Eye.Y += 1000;
    EXPECT_FALSE(WithinDetailRange(*runtime, overview));
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(both, 1, 0, frame));
    EXPECT_TRUE(frame.WantsDetails);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 0u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 12u);
    EXPECT_EQ(frame.VisiblePages, 2u);
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto first = frame.Reads.front();
    EXPECT_EQ(first.Page, 0u);
    EXPECT_TRUE(runtime->CompleteRead(first, kFirstPage));

    ASSERT_TRUE(runtime->Prepare(both, 2, 0, frame));
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 12u);
    ASSERT_TRUE(runtime->CompleteUpload(first, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(both, 3, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, first);
    EXPECT_EQ(frame.DetailedDraws.front().PlacementCount, 1u);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 6u);

    ForestPlacementView secondOnly = both;
    secondOnly.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(runtime->Prepare(secondOnly, 4, 2, frame));
    EXPECT_EQ(frame.VisiblePages, 1u);
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_TRUE(runtime->Prepare(secondOnly, 5, 3, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto second = frame.Reads.front();
    EXPECT_EQ(second.Page, 1u);
    EXPECT_EQ(second.Slot, first.Slot);
    EXPECT_NE(second.Generation, first.Generation);
    EXPECT_FALSE(runtime->CompleteUpload(first, ForestUploadResult::Published));

    // An invalid frame cannot lose the pending read or send it to the slot twice.
    EXPECT_FALSE(runtime->Prepare(secondOnly, 5, 3, frame));
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(runtime->CompleteRead(second, kSecondPage));
    EXPECT_TRUE(runtime->CompleteUpload(second, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(secondOnly, 6, 4, frame));
    EXPECT_TRUE(frame.StandDraws.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().PlacementCount, 2u);
    // Page 1 holds roots at (16, 23, 30) and (320, 0, 600) centimeters.
    const auto roots = runtime->ReadyRootBounds(second);
    ASSERT_TRUE(roots);
    EXPECT_EQ(roots->MinX, 16u);
    EXPECT_EQ(roots->MinY, 0u);
    EXPECT_EQ(roots->MinZ, 30u);
    EXPECT_EQ(roots->MaxX, 320u);
    EXPECT_EQ(roots->MaxY, 23u);
    EXPECT_EQ(roots->MaxZ, 600u);

    runtime->ResetResidency();
    ASSERT_TRUE(runtime->Prepare(secondOnly, 1, 0, frame));
    EXPECT_TRUE(frame.WantsDetails);
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    ASSERT_EQ(frame.Reads.size(), 1u);
    EXPECT_EQ(frame.Reads.front().Page, second.Page);
    EXPECT_NE(frame.Reads.front().Generation, second.Generation);
    EXPECT_FALSE(runtime->CompleteRead(second, kSecondPage));
}

TEST_F(ForestPlacementRuntimeTest, ReturningViewReusesAReadyPageAndExtendsItsReaderFence)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "camera-return.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1, .KeepCpuCopy = true}, error);
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
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));
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

TEST_F(ForestPlacementRuntimeTest, AllowsStandOnly)
{
    using namespace Engine::Renderer;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    const auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 0, .ReadsPerFrame = 4}, error);
    ASSERT_TRUE(runtime) << error;
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare({{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f}, 1, 0, frame));
    EXPECT_FALSE(frame.WantsDetails);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 0u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 12u);
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    EXPECT_FALSE(runtime->CompleteRead({0, 0, 0}, {}));
    EXPECT_FALSE(runtime->CompleteUpload({0, 0, 0}, ForestUploadResult::Published));
}

TEST_F(ForestPlacementRuntimeTest, ColdDetailDemandRequiresAVisiblePageWithinLookahead)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "cold.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 2, .ReadsPerFrame = 2}, error);
    ASSERT_TRUE(runtime) << error;

    ForestPlacementView view{{512'000'032.0, 8.0, 512'000'001.0}, {}, 10.0f};
    // The root covers the middle of the gap. Neither page is within range.
    EXPECT_FALSE(WithinDetailRange(*runtime, view));
    EXPECT_FALSE(runtime->CanPrefetchDetails(view));
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, plan));
    EXPECT_FALSE(plan.WantsDetails);
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    view.DetailDistance = 11.0f;
    EXPECT_TRUE(runtime->CanPrefetchDetails(view));
    EXPECT_FALSE(WithinDetailRange(*runtime, view));
    view.EyeRelativePlanes[0] = {1, 0, 0, -15};
    EXPECT_FALSE(runtime->CanPrefetchDetails(view));
    view.EyeRelativePlanes = {};
    view.DetailDistance = 25.0f;
    EXPECT_TRUE(WithinDetailRange(*runtime, view));
    view.EyeRelativePlanes[0] = {1, 0, 0, -15};
    EXPECT_TRUE(WithinDetailRange(*runtime, view)); // Only the second page remains visible.
    view.DetailDistance = 15.0f;
    EXPECT_FALSE(WithinDetailRange(*runtime, view)); // The near page is outside the frustum.
    view.EyeRelativePlanes[0] = {1, 0, 0, -100};
    view.DetailDistance = 100.0f;
    EXPECT_FALSE(WithinDetailRange(*runtime, view));
}

TEST_F(ForestPlacementRuntimeTest, PrefetchKeepsTheStandUntilThePageEntersTheDetailRange)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "detail-lookahead.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;

    ForestPlacementView view{{512'000'000.0 - 34.0, 8.0, 512'000'001.0}, {}, 25.0f};
    ASSERT_FALSE(WithinDetailRange(*runtime, view));
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
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));

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
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .TransitionSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    ASSERT_EQ(runtime->SlotCount(), 2u);

    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 25.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto first = frame.Reads.front();
    ASSERT_EQ(first.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(first, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(first, ForestUploadResult::Published));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, first);

    view.Eye.X = 512'000'104.0;
    ASSERT_TRUE(runtime->CanPrefetchDetails(view));
    ASSERT_FALSE(WithinDetailRange(*runtime, view));
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
    ASSERT_TRUE(runtime->CompleteUpload(second, ForestUploadResult::Published));

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
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;

    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto oldRead = plan.Reads.front();
    ASSERT_EQ(oldRead.Page, 0u);

    view.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(runtime->Prepare(view, 2, 0, plan));
    ASSERT_EQ(plan.CancelReads.size(), 1u);
    EXPECT_EQ(plan.CancelReads.front(), oldRead);
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto newRead = plan.Reads.front();
    EXPECT_EQ(newRead.Page, 1u);
    EXPECT_EQ(newRead.Slot, oldRead.Slot);
    EXPECT_NE(newRead.Generation, oldRead.Generation);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);

    EXPECT_FALSE(runtime->CompleteRead(oldRead, kFirstPage));
    EXPECT_TRUE(runtime->CompleteRead(newRead, kSecondPage));
    EXPECT_TRUE(runtime->CompleteUpload(newRead, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(view, 3, 2, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, newRead);
    EXPECT_TRUE(plan.StandDraws.empty());
}

TEST_F(ForestPlacementRuntimeTest, StreamRejectsArrivedBytesWhenTheCameraRetiresThePage)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(runtime);
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
    ASSERT_TRUE(runtime->CompleteUpload(second, ForestUploadResult::Published));
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
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 2, .ReadsPerFrame = 2}, error);
    ASSERT_TRUE(runtime) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(runtime);
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
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 2, .ReadsPerFrame = 2}, error);
    ASSERT_TRUE(runtime) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(runtime);
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
    EXPECT_FALSE(runtime->CanRecordUpload(retired));
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
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 2, .ReadsPerFrame = 2}, error);
    ASSERT_TRUE(runtime) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(runtime);
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
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    AssetManager unavailable;
    ForestPlacementStream stream(runtime);
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
    // The first failure waits one frame, so frame 3 reads page 0 again.
    ASSERT_TRUE(stream.Prepare(unavailable, view, 3, 2, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    // The second failure waits two frames. The next demand pass skips page 0, so page 1 takes the slot.
    ASSERT_TRUE(stream.Prepare(unavailable, view, 4, 3, plan, uploads));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_TRUE(stream.Prepare(unavailable, view, 5, 4, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
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
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    ForestPlacementFrame plan;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    EXPECT_FALSE(runtime->CanRecordUpload(ticket));
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    EXPECT_TRUE(runtime->CanRecordUpload(ticket));
    ForestPlacementUpload upload{ticket, kFirstPage};
    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    ForestPlacementGpuUploads uploader(runtime, {}, 1);
    EXPECT_FALSE(uploader.Record(graph, std::span(&upload, 1)));
    EXPECT_FALSE(runtime->CanRecordUpload(ticket));
    EXPECT_FALSE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(view, 2, 1, plan));
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    // A failed upload releases the slot; the page reads again after its one-frame retry wait.
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    EXPECT_EQ(plan.Reads.front().Slot, ticket.Slot);
    EXPECT_NE(plan.Reads.front().Generation, ticket.Generation);
}

TEST_F(ForestPlacementRuntimeTest, StationaryDemandStillRetriesAndKeepsOtherViewScratchSeparate)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
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
    // The same view retries page 0 at its retry frame without a new demand pass.
    ASSERT_TRUE(runtime->Prepare(near, 2, 1, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    const auto page = plan.Reads.front();
    ASSERT_TRUE(runtime->CompleteRead(page, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(page, ForestUploadResult::Published));
    runtime->GatherStand(otherView, stand);
    ASSERT_TRUE(runtime->Prepare(near, 3, 2, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, page);
    EXPECT_EQ(plan.VisiblePages, 2u);
}

TEST_F(ForestPlacementRuntimeTest, PrefetchReusesASlotWhosePageLeftTheViewAfterItsLastDraw)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "prefetch.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 25.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto first = frame.Reads.front();
    ASSERT_TRUE(runtime->CompleteRead(first, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(first, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);

    // Page 0 leaves every band and page 1 is in the prefetch band only.
    view.Eye.X = 512'000'104.0;
    ASSERT_TRUE(runtime->CanPrefetchDetails(view));
    ASSERT_FALSE(WithinDetailRange(*runtime, view));
    // Frame 2 drew page 0, so its slot stays until frame 2 is complete.
    ASSERT_TRUE(runtime->Prepare(view, 3, 1, frame));
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_TRUE(runtime->Prepare(view, 4, 2, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    EXPECT_EQ(frame.Reads.front().Page, 1u);
    EXPECT_EQ(frame.Reads.front().Slot, first.Slot);
}

TEST_F(ForestPlacementRuntimeTest, OnlyAPublishedUploadExposesItsPlacements)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "stages.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 2, .ReadsPerFrame = 2, .KeepCpuCopy = true}, error);
    ASSERT_TRUE(runtime) << error;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 2u);
    const auto first = frame.Reads[0];
    const auto second = frame.Reads[1];
    ASSERT_TRUE(runtime->CompleteRead(first, first.Page == 0 ? std::span<const uint8>(kFirstPage) : kSecondPage));
    EXPECT_TRUE(runtime->ReadyPlacements(first).empty());
    EXPECT_FALSE(runtime->ReadyRootBounds(first));
    ASSERT_TRUE(runtime->CompleteRead(second, second.Page == 0 ? std::span<const uint8>(kFirstPage) : kSecondPage));

    // An abandoned upload frees the slot without a retry delay: the next frame reads the page again.
    EXPECT_FALSE(runtime->CompleteUpload(second, ForestUploadResult::Abandoned));
    EXPECT_TRUE(runtime->ReadyPlacements(second).empty());
    ASSERT_TRUE(runtime->CompleteUpload(first, ForestUploadResult::Published));
    EXPECT_EQ(runtime->ReadyPlacements(first).size(), m_Pages[first.Page].Count);
    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    EXPECT_EQ(frame.Reads.front().Page, second.Page);
}

} // namespace
} // namespace GameEngine::PageStreaming
