// RGReadbackRing token-based completion contract: a slot is readable when the
// writing frame's submission token has signaled — no warmup window, no
// every-frame cadence requirement, no Reset(). Pins the pending lifecycle
// (register -> stamp -> signal -> consume), newest-wins supersession, and the
// dead-declare drop (a frame re-begun before its stamp never recorded).

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;

namespace
{
std::unique_ptr<IDevice> MakeDevice()
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    dd.enableDebugLayer = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

TextureDesc TinyColorDesc()
{
    TextureDesc d;
    d.width = 8;
    d.height = 8;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget);
    return d;
}

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 4096) {}
};

struct Meta
{
    int Value = 0;
};

BufferDesc SlotDesc()
{
    BufferDesc bd{};
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    // Readback: the ring exists to be read on the CPU, and Readback is the
    // class that asks for a host-cached type.
    bd.memoryUsage = BufferMemoryUsage::Readback;
    return bd;
}

// Minimal submittable frame: one cleared color pass so Execute() mints a
// graphics submission (and therefore a SubmissionToken).
void RunTrivialFrame(RGFrame& frame)
{
    RGTexture color = frame.CreateTexture("RingTest.Color", TinyColorDesc());
    frame.AddPass(
        "RingTest.Touch", 0,
        [&](RGPassBuilder& p)
        {
            RGAttachmentOps ops{};
            ops.Load = RGLoadOp::Clear;
            p.AttachColor(0, color, ops);
        },
        [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
}
} // namespace

TEST(RGReadbackRingTests, TokenGatedCompletionAndConsume)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(dev.get(), SlotDesc(), 3));

        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        ASSERT_TRUE(ring.BeginWrite(frame, Meta{7}).IsValid());

        // Unstamped: not readable no matter how much time passes.
        Meta out{};
        EXPECT_EQ(ring.MapNewestReady(dev.get(), &out), nullptr)
            << "a pending without its submission token must not be readable";

        RunTrivialFrame(frame);
        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        dev->WaitForIdle();

        const void* mapped = ring.MapNewestReady(dev.get(), &out);
        ASSERT_NE(mapped, nullptr) << "signaled token must make the slot readable";
        EXPECT_EQ(out.Value, 7) << "payload travels with the slot";
        ring.Unmap(dev.get());

        EXPECT_EQ(ring.MapNewestReady(dev.get(), &out), nullptr)
            << "a consumed pending must not be readable twice";

        ring.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(RGReadbackRingTests, DropPendingsDiscardsInFlightResults)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(dev.get(), SlotDesc(), 3));

        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        ASSERT_TRUE(ring.BeginWrite(frame, Meta{42}).IsValid());
        RunTrivialFrame(frame);
        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        dev->WaitForIdle();

        // Signaled and readable — but the owner invalidates first.
        ring.DropPendings();

        Meta out{};
        EXPECT_EQ(ring.MapNewestReady(dev.get(), &out), nullptr)
            << "invalidated in-flight results must not resolve as fresh";

        ring.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(RGReadbackRingTests, RetiredStreamPendingCannotBeStampedByRecycledFrame)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(dev.get(), SlotDesc(), 3));

        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        // A young window declares at index 5 and dies before Execute: the
        // pending stays unstamped, and the stream is retired (window close).
        frame.BeginFrame(5);
        ASSERT_TRUE(ring.BeginWrite(frame, Meta{13}).IsValid());
        ring.OnFrameStreamRetired(frame);

        // A new stream lands at the SAME address and its first submitted
        // frame index COINCIDES — the exact aliasing the retirement purge
        // exists to defeat. Without it, this stamp would mark the dead
        // pending complete for a submission that never recorded its write.
        frame.BeginFrame(5);
        RunTrivialFrame(frame);
        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        dev->WaitForIdle();

        Meta out{};
        EXPECT_EQ(ring.MapNewestReady(dev.get(), &out), nullptr)
            << "a retired stream's unstamped pending must never resolve";

        ring.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(RGReadbackRingTests, NewestReadyWinsAndOlderIsSuperseded)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(dev.get(), SlotDesc(), 3));

        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        for (int i = 0; i < 2; ++i)
        {
            frame.BeginFrame(static_cast<uint64_t>(i));
            ASSERT_TRUE(ring.BeginWrite(frame, Meta{10 + i}).IsValid());
            RunTrivialFrame(frame);
            ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        }
        dev->WaitForIdle();

        Meta out{};
        const void* mapped = ring.MapNewestReady(dev.get(), &out);
        ASSERT_NE(mapped, nullptr);
        EXPECT_EQ(out.Value, 11) << "the newest signaled slot wins";
        ring.Unmap(dev.get());

        EXPECT_EQ(ring.MapNewestReady(dev.get(), &out), nullptr)
            << "the older slot was superseded, not queued";

        ring.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(RGReadbackRingTests, DeadDeclareIsDroppedOnStamp)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(dev.get(), SlotDesc(), 3));

        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        // Register against incarnation 0, then re-begin the frame WITHOUT
        // submitting: the declaration died with the old graph.
        frame.BeginFrame(0);
        ASSERT_TRUE(ring.BeginWrite(frame, Meta{99}).IsValid());

        frame.BeginFrame(1);
        RunTrivialFrame(frame);
        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        dev->WaitForIdle();

        Meta out{};
        EXPECT_EQ(ring.MapNewestReady(dev.get(), &out), nullptr)
            << "a pending whose declare died before submit must be dropped, not "
               "stamped with a token from a submission that never recorded it";

        ring.Destroy(dev.get());
    }
    dev->Shutdown();
}

// A slot that a dispatch writes needs Storage; a slot the CPU maps needs the
// Readback class. WebGPU is the one backend where those cannot be the same
// allocation (MapRead composes with CopyDst and nothing else), so the ring
// splits the slot and RecordResolve copies one half into the other. The split
// is invisible to callers, which is exactly what this pins: the same
// BeginWrite -> write -> submit -> MapNewestReady sequence, on a device that
// reports the restriction, returns the bytes the GPU wrote.
TEST(RGReadbackRingTests, StorageSlotResolvesWhereStorageCannotBeMapped)
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::WebGPU;
    dd.enableSwapchain = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        GTEST_SKIP() << "No WebGPU device available";
    if (dev->GetCapabilities().supportsMappableStorageBuffers)
        GTEST_SKIP() << "device maps storage buffers directly — nothing to split";
    {
        FramePools pools(dev.get());
        RGReadbackRing<Meta> ring;
        BufferDesc bd = SlotDesc();
        bd.usage |= static_cast<uint32_t>(BufferUsage::TransferDst); // FillBuffer below
        ASSERT_TRUE(ring.Init(dev.get(), bd, 3))
            << "the ring must allocate a split slot rather than fail";
        ASSERT_TRUE(ring.SlotsAreSplit())
            << "a Storage slot on a device that cannot map one must be split in two";

        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        const BufferHandle slot = ring.BeginWrite(frame, Meta{5});
        ASSERT_TRUE(slot.IsValid());

        // The half handed out has to be bindable AS a storage buffer — the
        // thing a single Readback allocation cannot offer here, and the reason
        // the dispatch that owns the real slot was failing.
        DescriptorSetLayoutDesc layoutDesc{};
        DescriptorBinding storageBinding{};
        storageBinding.binding = 0;
        storageBinding.type = DescriptorType::StorageBuffer;
        storageBinding.shaderStages = kShaderStageCompute;
        layoutDesc.bindings.push_back(storageBinding);
        DescriptorSetDesc setDesc{};
        setDesc.layout = layoutDesc;
        setDesc.debugName = "RingTest.SplitStorageBind";
        const DescriptorSetHandle set = dev->CreateDescriptorSet(setDesc);
        ASSERT_TRUE(set.IsValid());
        dev->UpdateStorageBufferBinding(set, 0, slot, 0, bd.size);

        // Stands in for the depth-reduce dispatch's output: content in the
        // buffer BeginWrite handed out, which only the resolve can move into
        // the half the CPU maps. (A GPU-side fill would be the closer stand-in,
        // but WebGPU's only fill is a zero clear, which cannot be told apart
        // from an unresolved slot.)
        constexpr uint32_t kMarker = 0xABCDEF01u;
        dev->UpdateBuffer(slot, 0, sizeof(kMarker), &kMarker);

        // Attachment-less, like the SDSM depth-reduce pass that owns the real
        // slot: buffer work belongs outside a render-pass encoder.
        frame.AddPass(
            "RingTest.SplitWrite", 0, [](RGPassBuilder& p) { p.PreventCulling(); },
            [&ring, slot](RGContext& ctx)
            {
                ctx.Cmd->Barrier(ResourceBarrier::CreateBufferBarrier(
                    slot, ResourceState::CopyDest, ResourceState::UnorderedAccess));
                ring.RecordResolve(ctx.Cmd, slot);
            });
        // A cleared color pass so Execute() mints a graphics submission token.
        RunTrivialFrame(frame);

        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        dev->WaitForIdle();

        Meta out{};
        const void* mapped = ring.MapNewestReady(dev.get(), &out);
        ASSERT_NE(mapped, nullptr) << "the mappable half must be readable after the resolve";
        uint32_t readBack = 0;
        std::memcpy(&readBack, mapped, sizeof(readBack));
        EXPECT_EQ(readBack, kMarker) << "the resolve must carry the GPU's write across";
        EXPECT_EQ(out.Value, 5) << "payload still travels with the slot";
        ring.Unmap(dev.get());

        ring.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(RGReadbackRingTests, PreservedResultsBackPressureAndConsumeIndividually)
{
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(device.get(), SlotDesc(), 2, RGReadbackRetention::PreserveUnread));
        RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);
        uint64_t firstId = 0, secondId = 0;
        const auto first = ring.ReserveWrite(frame, {7}, firstId);
        const auto second = ring.ReserveWrite(frame, {9}, secondId);
        ASSERT_TRUE(first.IsValid());
        ASSERT_TRUE(second.IsValid());
        EXPECT_NE(first, second);
        EXPECT_NE(firstId, secondId);
        EXPECT_FALSE(ring.BeginWrite(frame, {11}).IsValid());
        EXPECT_TRUE(ring.HasPendingWrite(frame, first, firstId));
        EXPECT_TRUE(ring.HasPendingWrite(frame, second, secondId));
        ASSERT_TRUE(ring.MarkRecorded(first, firstId));
        ASSERT_TRUE(ring.MarkRecorded(second, secondId));
        RunTrivialFrame(frame);
        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        device->WaitForIdle();
        Meta metadata{};
        ASSERT_NE(ring.MapOldestReady(device.get(), &metadata), nullptr);
        EXPECT_EQ(metadata.Value, 7);
        ring.Unmap(device.get());
        ASSERT_NE(ring.MapOldestReady(device.get(), &metadata), nullptr);
        EXPECT_EQ(metadata.Value, 9);
        ring.Unmap(device.get());
        EXPECT_EQ(ring.MapOldestReady(device.get(), &metadata), nullptr);
        ring.Destroy(device.get());
    }
    device->Shutdown();
}

TEST(RGReadbackRingTests, CancellationKeepsSubmittedResultsAndInvalidatesOldReservations)
{
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(device.get(), SlotDesc(), 2, RGReadbackRetention::PreserveUnread));
        RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        RGUploadRing otherUploads(device.get(), 2, 4096);
        RGFrame other(device.get(), &pools.Persistent, &pools.Transient, &otherUploads);
        frame.BeginFrame(7);
        uint64_t submittedId = 0;
        const auto submitted = ring.ReserveWrite(frame, {7}, submittedId);
        ASSERT_TRUE(submitted.IsValid());
        ASSERT_TRUE(ring.MarkRecorded(submitted, submittedId));
        RunTrivialFrame(frame);
        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        device->WaitForIdle();
        frame.BeginFrame(8);
        uint64_t abortedId = 0;
        const auto aborted = ring.ReserveWrite(frame, {8}, abortedId);
        ASSERT_TRUE(aborted.IsValid());
        other.BeginFrame(8);
        ring.CancelUnsubmitted(other);
        EXPECT_TRUE(ring.HasPendingWrite(frame, aborted, abortedId));
        ring.CancelUnsubmitted(frame);
        EXPECT_FALSE(ring.HasPendingWrite(frame, aborted, abortedId));
        Meta metadata{};
        ASSERT_NE(ring.MapOldestReady(device.get(), &metadata), nullptr);
        EXPECT_EQ(metadata.Value, 7);
        ring.Unmap(device.get());
        uint64_t replacementId = 0;
        const auto replacement = ring.ReserveWrite(frame, {9}, replacementId);
        EXPECT_TRUE(replacement.IsValid());
        EXPECT_TRUE(ring.HasPendingWrite(frame, replacement, replacementId));
        EXPECT_FALSE(ring.HasPendingWrite(frame, submitted, submittedId));
        EXPECT_FALSE(ring.HasPendingWrite(frame, replacement, abortedId));
        ring.Destroy(device.get());
    }
    device->Shutdown();
}

TEST(RGReadbackRingTests, UnrecordedReservationCannotBecomeReadyWithAnUnrelatedSubmission)
{
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RGReadbackRing<Meta> ring;
        ASSERT_TRUE(ring.Init(device.get(), SlotDesc(), 1, RGReadbackRetention::PreserveUnread));
        RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);
        uint64_t unrecorded = 0;
        const auto output = ring.ReserveWrite(frame, {7}, unrecorded);
        ASSERT_TRUE(output.IsValid());
        EXPECT_TRUE(ring.HasPendingWrite(frame, output, unrecorded));
        RunTrivialFrame(frame); // the reserved buffer has no writer
        ring.OnFrameSubmitted(frame, frame.SubmissionToken());
        device->WaitForIdle();
        Meta metadata{};
        const void* unexpected = ring.MapOldestReady(device.get(), &metadata);
        EXPECT_EQ(unexpected, nullptr);
        if (unexpected)
            ring.Unmap(device.get());
        EXPECT_FALSE(ring.HasPendingWrite(frame, output, unrecorded));
        uint64_t replacement = 0;
        const auto next = ring.ReserveWrite(frame, {9}, replacement);
        EXPECT_EQ(next, output);
        EXPECT_NE(replacement, unrecorded);
        EXPECT_FALSE(ring.MarkRecorded(next, unrecorded));
        EXPECT_TRUE(ring.MarkRecorded(next, replacement));
        ring.Destroy(device.get());
    }
    device->Shutdown();
}
