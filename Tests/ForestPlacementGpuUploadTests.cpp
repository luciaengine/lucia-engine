// The forest slot uploads on a headless device: a copy that never submits is abandoned and its page
// reads again, a submitted copy publishes its slot only after its queue fence, an outstanding
// copy blocks only its own ticket, and an unknown read fence drops the shadow demand.
// Device-gated: each test skips without a Vulkan device.

#include "PackedPlacementTestArchive.h"

#include "Assets/AssetManager.h"
#include "ForestPlacementGpuUploads.h"
#include "ForestPlacementRenderState.h"
#include "ForestPlacementRuntime.h"
#include "ForestPlacementUploadPlan.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Tests/ScopedEnvVar.h"
#if defined(RENDERING_HAS_VULKAN)
#include "Vulkan/VulkanDevice/VulkanDevice.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>

namespace GameEngine::PageStreaming
{
namespace
{
using namespace Engine::Renderer;
using namespace Rendering;
using namespace Rendering::RenderGraph;

constexpr uint64 kUploadRingBytes = 64 * 1024;

#if defined(RENDERING_HAS_VULKAN)
// Reports a timeline value no higher than HeldValue, so a test decides when a submission is complete.
class HeldTimelineDevice final : public VulkanDevice
{
public:
    std::optional<uint64_t> HeldValue;

    bool GetTimelineSemaphoreValue(SemaphoreHandle semaphore, uint64_t& value) const override
    {
        if (!VulkanDevice::GetTimelineSemaphoreValue(semaphore, value))
            return false;
        if (HeldValue)
            value = std::min(value, *HeldValue);
        return true;
    }
};
#endif

std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    // Every device of this process is headless.
    Rendering::Tests::SetEnvVar("GE_HEADLESS_TEST", "1");
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
#if defined(RENDERING_HAS_VULKAN)
    std::unique_ptr<IDevice> device = std::make_unique<HeldTimelineDevice>();
#else
    auto device = DeviceFactory::CreateDevice(desc);
#endif
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, kUploadRingBytes) {}
};

class ForestPlacementGpuUploadTest : public PackedPlacementArchiveTest
{
protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        const auto file = m_Scratch.Path() / "gpu.geforest";
        WriteStore(file);
        std::string error;
        m_Runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
        ASSERT_TRUE(m_Runtime) << error;
        const BufferDesc desc{.size = kPlacementSlotBytes,
                              .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst),
                              .memoryUsage = BufferMemoryUsage::DeviceLocal,
                              .debugName = "ForestPlacementGpuUploadTest.Slots"};
        m_Slots = m_Device->CreateBuffer(desc);
        ASSERT_TRUE(m_Slots.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Slots.IsValid())
            m_Device->DestroyBuffer(m_Slots);
        m_Device->Shutdown();
    }

    // Plans frame 1 and verifies its read of page 0, so its upload can be recorded.
    ForestPageTicket ReadFirstPage()
    {
        ForestPlacementFrame plan;
        EXPECT_TRUE(m_Runtime->Prepare(m_View, 1, 0, {}, plan));
        EXPECT_EQ(plan.Reads.size(), 1u);
        const ForestPageTicket ticket = plan.Reads.front();
        EXPECT_EQ(ticket.Page, 0u);
        EXPECT_TRUE(m_Runtime->CompleteRead(ticket, kFirstPage));
        return ticket;
    }

    std::unique_ptr<IDevice> m_Device;
    std::shared_ptr<ForestPlacementRuntime> m_Runtime;
    BufferHandle m_Slots;
    const ForestPlacementView m_View{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
};

TEST_F(ForestPlacementGpuUploadTest, AnUnsubmittedCopyIsAbandonedAndItsPageReadsAgain)
{
    const ForestPageTicket ticket = ReadFirstPage();
    FramePools pools(m_Device.get());
    RGFrame graph(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    ASSERT_TRUE(m_Device->BeginFrame());
    graph.BeginFrame(0);
    ForestPlacementGpuUploads uploader(m_Runtime, m_Slots, 1);
    const ForestPlacementUpload upload{ticket, kFirstPage};
    ASSERT_TRUE(uploader.Record(graph, std::span(&upload, 1)));
    EXPECT_EQ(uploader.DrainCompleted(*m_Device), 0u);

    uploader.FinishFrame(graph);
    graph.DiscardRecordedPasses();
    EXPECT_FALSE(m_Runtime->CanRecordUpload(ticket));
    ForestPlacementFrame plan;
    ASSERT_TRUE(m_Runtime->Prepare(m_View, 2, 1, {}, plan));
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    EXPECT_NE(plan.Reads.front().Generation, ticket.Generation);
}

TEST_F(ForestPlacementGpuUploadTest, ASubmittedCopyPublishesItsSlotAfterItsFence)
{
    const ForestPageTicket ticket = ReadFirstPage();
    FramePools pools(m_Device.get());
    RGFrame graph(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    ASSERT_TRUE(m_Device->BeginFrame());
    graph.BeginFrame(0);
    ForestPlacementGpuUploads uploader(m_Runtime, m_Slots, 1);
    const ForestPlacementUpload upload{ticket, kFirstPage};
    ASSERT_TRUE(uploader.Record(graph, std::span(&upload, 1)));
    // A second record of the same ticket is refused while its copy is outstanding.
    EXPECT_FALSE(uploader.Record(graph, std::span(&upload, 1)));
    EXPECT_EQ(uploader.DrainCompleted(*m_Device), 0u);

    graph.Execute();
    uploader.FinishFrame(graph);
    m_Device->WaitForIdle();
#if defined(RENDERING_HAS_VULKAN)
    auto& held = static_cast<HeldTimelineDevice&>(*m_Device);
    held.HeldValue = 0;
    EXPECT_EQ(uploader.DrainCompleted(*m_Device), 0u) << "a copy published before its fence completed";
    held.HeldValue.reset();
#endif
    ASSERT_EQ(m_Device->QueryGpuSyncToken(graph.SubmissionToken()), IDevice::GpuSyncStatus::Complete);
    EXPECT_EQ(uploader.DrainCompleted(*m_Device), 1u);
    EXPECT_EQ(uploader.DrainCompleted(*m_Device), 0u);
    ForestPlacementFrame plan;
    ASSERT_TRUE(m_Runtime->Prepare(m_View, 2, 1, {}, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, ticket);
}

TEST_F(ForestPlacementGpuUploadTest, AnUnknownReadFenceDropsShadowDemand)
{
    std::string error;
    const auto runtime = ForestPlacementRuntime::Open(m_Scratch.Path() / "gpu.geforest",
                                                      {.DetailSlots = 1, .ReadsPerFrame = 1, .ShadowSlots = 1}, error);
    ASSERT_TRUE(runtime) << error;
    const BufferDesc desc{.size = runtime->SlotCount() * kPlacementSlotBytes,
                          .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst),
                          .memoryUsage = BufferMemoryUsage::DeviceLocal,
                          .debugName = "ForestPlacementGpuUploadTest.ShadowSlots"};
    const BufferHandle slots = m_Device->CreateBuffer(desc);
    ASSERT_TRUE(slots.IsValid());
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    FramePools pools(m_Device.get());
    RGFrame graph(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    // This graph never submits, so its submission token is unknown.
    RGFrame unsubmitted(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    // The camera sees no page; cascade 0 wants page 0.
    ForestPlacementView blind = m_View;
    blind.EyeRelativePlanes[0] = {0, 0, -1, -400};
    constexpr std::array<ForestShadowPage, 1> light{{{0, 1}}};
    ForestPlacementFrame plan;
    {
        ForestPlacementRenderState state(runtime, slots, runtime->SlotCount());
        // Frame 1 reads page 0, frame 2 copies it into its slot.
        for (uint64 frame = 0; frame < 2; ++frame)
        {
            ASSERT_TRUE(m_Device->BeginFrame());
            graph.BeginFrame(frame);
            ASSERT_TRUE(state.Prepare(assets, *m_Device, graph, blind, plan, light));
            graph.Execute();
            state.Finish(graph, true);
            m_Device->WaitForIdle();
        }
        ASSERT_TRUE(m_Device->BeginFrame());
        unsubmitted.BeginFrame(2);
        ASSERT_TRUE(state.Prepare(assets, *m_Device, unsubmitted, blind, plan, light));
        ASSERT_EQ(plan.ShadowDraws.size(), 1u);
        state.Finish(unsubmitted, true);
        unsubmitted.DiscardRecordedPasses();

        ASSERT_TRUE(m_Device->BeginFrame());
        unsubmitted.BeginFrame(3);
        ASSERT_TRUE(state.Prepare(assets, *m_Device, unsubmitted, blind, plan, light));
        EXPECT_TRUE(plan.ShadowDraws.empty());
        EXPECT_TRUE(plan.Reads.empty());
        state.Finish(unsubmitted, false);
        unsubmitted.DiscardRecordedPasses();
    }
    m_Device->WaitForIdle();
    m_Device->DestroyBuffer(slots);
}

#if defined(RENDERING_HAS_VULKAN)
TEST_F(ForestPlacementGpuUploadTest, AnOutstandingCopyBlocksOnlyItsOwnTicket)
{
    const ForestPageTicket stale = ReadFirstPage();
    FramePools pools(m_Device.get());
    RGFrame graph(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    ASSERT_TRUE(m_Device->BeginFrame());
    graph.BeginFrame(0);
    ForestPlacementGpuUploads uploader(m_Runtime, m_Slots, 1);
    const ForestPlacementUpload staleUpload{stale, kFirstPage};
    ASSERT_TRUE(uploader.Record(graph, std::span(&staleUpload, 1)));
    graph.Execute();
    uploader.FinishFrame(graph);
    m_Device->WaitForIdle();
    auto& held = static_cast<HeldTimelineDevice&>(*m_Device);
    held.HeldValue = 0;

    // A residency reset admits the page again into the same slot while the old copy is outstanding.
    m_Runtime->ResetResidency();
    const ForestPageTicket fresh = ReadFirstPage();
    ASSERT_EQ(fresh.Slot, stale.Slot);
    ASSERT_NE(fresh.Generation, stale.Generation);
    ASSERT_TRUE(m_Device->BeginFrame());
    graph.BeginFrame(1);
    const ForestPlacementUpload freshUpload{fresh, kFirstPage};
    EXPECT_TRUE(uploader.Record(graph, std::span(&freshUpload, 1)));
    graph.Execute();
    uploader.FinishFrame(graph);
    m_Device->WaitForIdle();
    held.HeldValue.reset();
    EXPECT_EQ(uploader.DrainCompleted(*m_Device), 1u);
}
#endif

} // namespace
} // namespace GameEngine::PageStreaming
