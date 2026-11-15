#include "Engine/Rendering/VirtualShadows/VirtualShadowRequestReuse.h"

#include <gtest/gtest.h>

using GameEngine::Engine::Renderer::Pipeline::Nodes::VirtualShadowRequestReuse;
using GameEngine::Rendering::ElisionInputBlob;

namespace
{
ElisionInputBlob Key(uint64_t value = 1)
{
    ElisionInputBlob blob;
    blob.Append(value);
    return blob;
}

void Submit(VirtualShadowRequestReuse& cache, uint64_t phase = 1, bool clear = false)
{
    const auto write = cache.PrepareWrite();
    EXPECT_NE(write.Ticket, 0u);
    EXPECT_EQ(write.Clear, clear);
    EXPECT_TRUE(cache.MarkSubmitted(write.Ticket, Key(phase), 7));
}

void Warm(VirtualShadowRequestReuse& cache)
{
    for (uint64_t frame = 0; frame < 4; ++frame)
    {
        const bool reused = cache.CanReuse(frame, Key(), Key(), 7, false, true);
        EXPECT_FALSE(reused);
        if (!reused)
            Submit(cache, 1, frame == 0 || frame == 3);
    }
}
}

TEST(VirtualShadowRequestReuse, RequiresAnActualSubmittedMaskAfterCpuInputsSettle)
{
    VirtualShadowRequestReuse cache;
    EXPECT_EQ(cache.PrepareWrite().Ticket, 0u);
    EXPECT_FALSE(cache.MarkSubmitted(0, Key(), 7));
    for (uint64_t frame = 0; frame < 5; ++frame)
        EXPECT_FALSE(cache.CanReuse(frame, Key(), Key(), 7, false, true));
    Submit(cache, 1, true);
    for (uint64_t frame = 5; frame <= 7; ++frame)
    {
        EXPECT_FALSE(cache.CanReuse(frame, Key(), Key(), 7, false, true));
        Submit(cache, 1, frame == 7);
    }
    EXPECT_TRUE(cache.CanReuse(8, Key(), Key(), 7, false, true));
}

TEST(VirtualShadowRequestReuse, AlternatingExactPhasesAccumulateBeforeEitherCanBeReused)
{
    VirtualShadowRequestReuse cache;
    uint64_t gpuMask = 0;
    for (uint64_t frame = 0; frame < 12; ++frame)
    {
        const uint64_t phase = frame % 2;
        const uint64_t desired = uint64_t{1} << phase;
        const bool reused = cache.CanReuse(frame, Key(), Key(phase), 7, false, true);
        EXPECT_EQ(reused, frame >= 5);
        if (!reused)
        {
            const auto write = cache.PrepareWrite();
            EXPECT_EQ(write.Clear, frame == 0 || frame == 3);
            if (write.Clear) gpuMask = 0;
            gpuMask |= desired;
            EXPECT_TRUE(cache.MarkSubmitted(write.Ticket, Key(phase), 7));
        }
        EXPECT_EQ(gpuMask & desired, desired) << "a reused phase must retain its actual GPU contribution";
    }
    EXPECT_EQ(gpuMask, 3u);
}

TEST(VirtualShadowRequestReuse, AnUnknownExactPhaseMustAppendEvenAfterTheDomainSettles)
{
    VirtualShadowRequestReuse cache;
    Warm(cache);
    EXPECT_FALSE(cache.CanReuse(4, Key(), Key(2), 7, false, true));
    Submit(cache, 2);
    EXPECT_TRUE(cache.CanReuse(5, Key(), Key(1), 7, false, true));
    EXPECT_TRUE(cache.CanReuse(6, Key(), Key(2), 7, false, true));
}

TEST(VirtualShadowRequestReuse, AbortedAppendForgetsProofAndClearsBeforeTheNextWrite)
{
    VirtualShadowRequestReuse cache;
    Warm(cache);
    EXPECT_FALSE(cache.CanReuse(4, Key(), Key(2), 7, false, true));
    const auto abandoned = cache.PrepareWrite();
    EXPECT_FALSE(abandoned.Clear);
    EXPECT_FALSE(cache.CanReuse(5, Key(), Key(1), 7, false, true));
    const auto retry = cache.PrepareWrite();
    EXPECT_TRUE(retry.Clear);
    EXPECT_FALSE(cache.MarkSubmitted(abandoned.Ticket, Key(2), 7));
    EXPECT_TRUE(cache.MarkSubmitted(retry.Ticket, Key(1), 7));
    EXPECT_FALSE(cache.CanReuse(6, Key(), Key(2), 7, false, true));
}

TEST(VirtualShadowRequestReuse, ChangedDomainInvalidatesReceiptsBeforeAnotherWriteBegins)
{
    VirtualShadowRequestReuse cache;
    EXPECT_FALSE(cache.CanReuse(0, Key(), Key(), 7, false, true));
    const auto abandoned = cache.PrepareWrite();
    EXPECT_FALSE(cache.CanReuse(1, Key(2), Key(), 7, false, true));
    EXPECT_FALSE(cache.MarkSubmitted(abandoned.Ticket, Key(), 7));
    Submit(cache, 1, true);
    for (uint64_t frame = 2; frame <= 4; ++frame)
    {
        EXPECT_FALSE(cache.CanReuse(frame, Key(2), Key(), 7, false, true));
        Submit(cache, 1, frame == 4);
    }
    EXPECT_TRUE(cache.CanReuse(5, Key(2), Key(), 7, false, true));
}

TEST(VirtualShadowRequestReuse, StaleSubmissionCannotValidateANewerOverwrite)
{
    VirtualShadowRequestReuse cache;
    EXPECT_FALSE(cache.CanReuse(0, Key(), Key(), 7, false, true));
    const auto old = cache.PrepareWrite();
    const auto current = cache.PrepareWrite();
    EXPECT_FALSE(cache.MarkSubmitted(old.Ticket, Key(), 7));
    EXPECT_TRUE(cache.MarkSubmitted(current.Ticket, Key(), 7));
    for (uint64_t frame = 1; frame <= 3; ++frame)
    {
        EXPECT_FALSE(cache.CanReuse(frame, Key(), Key(), 7, false, true));
        Submit(cache, 1, frame == 3);
    }
    EXPECT_TRUE(cache.CanReuse(4, Key(), Key(), 7, false, true));
}

TEST(VirtualShadowRequestReuse, RecreatedAllocationNeedsAFreshReceiptEvenIfItsIdRepeats)
{
    VirtualShadowRequestReuse cache;
    Warm(cache);
    EXPECT_FALSE(cache.CanReuse(4, Key(), Key(2), 7, false, true));
    const auto old = cache.PrepareWrite();
    EXPECT_FALSE(cache.CanReuse(5, Key(), Key(), 7, true, true));
    EXPECT_FALSE(cache.MarkSubmitted(old.Ticket, Key(2), 7));
    Submit(cache, 1, true);
    EXPECT_FALSE(cache.CanReuse(6, Key(), Key(2), 7, false, true));
}

TEST(VirtualShadowRequestReuse, FrameGapForgetsBothPhaseContributionsAndReceipts)
{
    VirtualShadowRequestReuse cache;
    Warm(cache);
    EXPECT_FALSE(cache.CanReuse(12, Key(), Key(), 7, false, true));
    Submit(cache, 1, true);
    EXPECT_FALSE(cache.CanReuse(13, Key(), Key(), 7, false, true));
}

TEST(VirtualShadowRequestReuse, DisabledAndDifferentAllocationsCannotReuseOrPublishProof)
{
    VirtualShadowRequestReuse cache;
    Warm(cache);
    EXPECT_FALSE(cache.CanReuse(4, Key(), Key(), 8, false, true));
    EXPECT_FALSE(cache.CanReuse(5, Key(), Key(), 7, false, false));
    const auto write = cache.PrepareWrite();
    EXPECT_TRUE(write.Clear);
    EXPECT_FALSE(cache.MarkSubmitted(write.Ticket, Key(), 7));
    EXPECT_FALSE(cache.CanReuse(6, Key(), Key(), 7, false, true));
    Submit(cache, 1, true);
    for (uint64_t frame = 7; frame <= 9; ++frame)
    {
        EXPECT_FALSE(cache.CanReuse(frame, Key(), Key(), 7, false, true));
        Submit(cache, 1, frame == 9);
    }
    EXPECT_TRUE(cache.CanReuse(10, Key(), Key(), 7, false, true));
}

TEST(VirtualShadowRequestReuse, BoundedHistoryEvictionLosesOnlyASkipOpportunity)
{
    VirtualShadowRequestReuse cache;
    for (uint64_t frame = 0; frame < 68; ++frame)
    {
        EXPECT_FALSE(cache.CanReuse(frame, Key(), Key(frame), 7, false, true));
        Submit(cache, frame, frame == 0 || frame == 3);
    }
    EXPECT_FALSE(cache.CanReuse(68, Key(), Key(3), 7, false, true));
    EXPECT_TRUE(cache.CanReuse(69, Key(), Key(67), 7, false, true));
}
