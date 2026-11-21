#include "Engine/Rendering/Impostors/ImpostorAtlasCache.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

ImpostorAtlasKey KeyFor(uint64 meshHash)
{
    ImpostorAtlasKey key;
    key.MeshHash = meshHash;
    key.MaterialHash = 7;
    return key;
}

} // namespace

TEST(ImpostorAtlasCache, AtlasBytesIncludeTheSeasonMask)
{
    // 64 frames x 128^2 pixels x (4 + 8 + 2) bytes.
    EXPECT_EQ(ImpostorAtlasBytes(KeyFor(1)), 64ull * 128 * 128 * 14);
}

TEST(ImpostorAtlasCache, FilteredAtlasesReserveEveryTileLevel)
{
    const auto standard = KeyFor(1);
    auto filtered = standard;
    filtered.FilterMips = true;
    EXPECT_NE(standard, filtered);
    EXPECT_EQ(ImpostorAtlasMipCount(standard), 1u);
    EXPECT_EQ(ImpostorAtlasMipCount(filtered), 6u);
    EXPECT_EQ(ImpostorAtlasBytes(filtered), 64ull * 21'840 * 14);

    ImpostorAtlasCache cache(ImpostorAtlasBytes(standard));
    std::vector<ImpostorAtlasKey> evicted;
    EXPECT_EQ(cache.Request(filtered, 1, evicted), ImpostorAtlasCache::State::Absent);
    EXPECT_EQ(cache.Request(standard, 1, evicted), ImpostorAtlasCache::State::Pending);
}

TEST(ImpostorAtlasCache, FilteredTilesStopBeforeLevelsCanMergeViews)
{
    auto compact = KeyFor(1);
    compact.FilterMips = true;
    compact.BakeDiffuseTransmission = true;
    compact.FrameSize = 64;
    EXPECT_EQ(ImpostorAtlasMipCount(compact), 5u);
    EXPECT_EQ(ImpostorAtlasBytes(compact), 64ull * 5'456 * 18);

    compact.FrameSize = 48;
    EXPECT_EQ(ImpostorAtlasMipCount(compact), 1u);
    EXPECT_EQ(ImpostorAtlasBytes(compact), 64ull * 48 * 48 * 18);
}

TEST(ImpostorAtlasCache, OnlyTransmittingAtlasesPayForTheExtraLayer)
{
    const auto standard = KeyFor(1);
    auto transmitting = standard;
    transmitting.BakeDiffuseTransmission = true;
    EXPECT_NE(standard, transmitting);
    EXPECT_EQ(ImpostorAtlasBytes(transmitting) - ImpostorAtlasBytes(standard), 64ull * 128 * 128 * 4);
    ImpostorAtlasCache cache(ImpostorAtlasBytes(standard));
    std::vector<ImpostorAtlasKey> evicted;
    EXPECT_EQ(cache.Request(transmitting, 1, evicted), ImpostorAtlasCache::State::Absent);
    EXPECT_EQ(cache.Request(standard, 1, evicted), ImpostorAtlasCache::State::Pending);
}

TEST(ImpostorAtlasCache, CompactTilesPreserveTheNumberOfTransmittingAtlases)
{
    auto standard = KeyFor(1);
    standard.BakeDiffuseTransmission = true;
    auto compact = standard;
    compact.FrameSize = 64;
    constexpr uint64 kStandardBudget = 256ull << 20;
    constexpr uint64 kCompactBudget = 64ull << 20;

    EXPECT_NE(standard, compact);
    EXPECT_EQ(ImpostorAtlasBytes(compact) * 4, ImpostorAtlasBytes(standard));
    EXPECT_EQ(kCompactBudget / ImpostorAtlasBytes(compact), kStandardBudget / ImpostorAtlasBytes(standard));

    ImpostorAtlasCache cache(kCompactBudget);
    std::vector<ImpostorAtlasKey> evicted;
    const uint64 atlasCount = kCompactBudget / ImpostorAtlasBytes(compact);
    for (uint64 index = 1; index <= atlasCount; ++index)
    {
        compact.MeshHash = index;
        EXPECT_EQ(cache.Request(compact, 1, evicted), ImpostorAtlasCache::State::Pending);
    }
    compact.MeshHash = atlasCount + 1;
    EXPECT_EQ(cache.Request(compact, 1, evicted), ImpostorAtlasCache::State::Absent);
    EXPECT_TRUE(evicted.empty());
}

TEST(ImpostorAtlasCache, ARequestThatFitsIsPendingUntilItsBakeIsReady)
{
    ImpostorAtlasCache cache(ImpostorAtlasBytes(KeyFor(1)) * 2);
    std::vector<ImpostorAtlasKey> evicted;
    EXPECT_EQ(cache.GetState(KeyFor(1)), ImpostorAtlasCache::State::Absent);
    EXPECT_EQ(cache.Request(KeyFor(1), 1, evicted), ImpostorAtlasCache::State::Pending);
    cache.MarkReady(KeyFor(1));
    EXPECT_EQ(cache.Request(KeyFor(1), 2, evicted), ImpostorAtlasCache::State::Ready);
    EXPECT_TRUE(evicted.empty());
    EXPECT_EQ(cache.GetUsedBytes(), ImpostorAtlasBytes(KeyFor(1)));
}

TEST(ImpostorAtlasCache, PendingBakeLosesDemandAfterOneIdleEpoch)
{
    const auto key = KeyFor(1);
    ImpostorAtlasCache cache(ImpostorAtlasBytes(key));
    std::vector<ImpostorAtlasKey> evicted;
    EXPECT_FALSE(cache.HasRecentPendingRequest(key, 10));
    ASSERT_EQ(cache.Request(key, 10, evicted), ImpostorAtlasCache::State::Pending);
    EXPECT_TRUE(cache.HasRecentPendingRequest(key, 10));
    EXPECT_TRUE(cache.HasRecentPendingRequest(key, 11));
    EXPECT_FALSE(cache.HasRecentPendingRequest(key, 12));

    cache.ReleasePending(key);
    EXPECT_EQ(cache.GetUsedBytes(), 0u);
    ASSERT_EQ(cache.Request(key, 12, evicted), ImpostorAtlasCache::State::Pending);
    EXPECT_TRUE(cache.HasRecentPendingRequest(key, 12));
    cache.MarkReady(key);
    EXPECT_FALSE(cache.HasRecentPendingRequest(key, 12));
}

TEST(ImpostorAtlasCache, PastTheBudgetTheLeastRecentlyUsedAtlasGoes)
{
    ImpostorAtlasCache cache(ImpostorAtlasBytes(KeyFor(1)) * 2);
    std::vector<ImpostorAtlasKey> evicted;
    cache.Request(KeyFor(1), 1, evicted);
    cache.Request(KeyFor(2), 2, evicted);
    cache.Request(KeyFor(1), 3, evicted); // 1 is now more recent than 2.
    EXPECT_EQ(cache.Request(KeyFor(3), 4, evicted), ImpostorAtlasCache::State::Pending);
    ASSERT_EQ(evicted.size(), 1u);
    EXPECT_EQ(evicted[0], KeyFor(2));
    EXPECT_EQ(cache.GetState(KeyFor(2)), ImpostorAtlasCache::State::Absent);
    EXPECT_EQ(cache.GetUsedBytes(), cache.GetBudgetBytes());
}

TEST(ImpostorAtlasCache, AnAtlasUsedInThisFrameIsNeverEvicted)
{
    ImpostorAtlasCache cache(ImpostorAtlasBytes(KeyFor(1)) * 2);
    std::vector<ImpostorAtlasKey> evicted;
    cache.Request(KeyFor(1), 5, evicted);
    cache.Request(KeyFor(2), 5, evicted);
    EXPECT_EQ(cache.Request(KeyFor(3), 5, evicted), ImpostorAtlasCache::State::Absent)
        << "both held atlases draw in this frame, so the third draws the mesh";
    EXPECT_TRUE(evicted.empty());
}

TEST(ImpostorAtlasCache, ARejectedRequestKeepsReusableAtlases)
{
    const auto oldest = KeyFor(1);
    const auto recent = KeyFor(2);
    auto transmitting = KeyFor(3);
    transmitting.BakeDiffuseTransmission = true;
    ImpostorAtlasCache cache(ImpostorAtlasBytes(oldest) * 2);
    std::vector<ImpostorAtlasKey> evicted;
    ASSERT_EQ(cache.Request(oldest, 1, evicted), ImpostorAtlasCache::State::Pending);
    ASSERT_EQ(cache.Request(recent, 2, evicted), ImpostorAtlasCache::State::Pending);
    cache.MarkReady(oldest);
    cache.MarkReady(recent);

    EXPECT_EQ(cache.Request(transmitting, 3, evicted), ImpostorAtlasCache::State::Absent);
    EXPECT_TRUE(evicted.empty());
    EXPECT_EQ(cache.GetState(oldest), ImpostorAtlasCache::State::Ready);
    EXPECT_EQ(cache.GetState(recent), ImpostorAtlasCache::State::Ready);
    EXPECT_EQ(cache.GetUsedBytes(), cache.GetBudgetBytes());

    EXPECT_EQ(cache.Request(transmitting, 5, evicted), ImpostorAtlasCache::State::Pending);
    EXPECT_EQ(evicted, (std::vector<ImpostorAtlasKey>{oldest, recent}));
    EXPECT_EQ(cache.GetUsedBytes(), ImpostorAtlasBytes(transmitting));
}

TEST(ImpostorAtlasCache, OversubscribedDemandKeepsTheSameReadyAtlasesAcrossFrames)
{
    ImpostorAtlasCache cache(ImpostorAtlasBytes(KeyFor(1)) * 2);
    std::vector<ImpostorAtlasKey> evicted;
    ASSERT_EQ(cache.Request(KeyFor(1), 1, evicted), ImpostorAtlasCache::State::Pending);
    ASSERT_EQ(cache.Request(KeyFor(2), 1, evicted), ImpostorAtlasCache::State::Pending);
    cache.MarkReady(KeyFor(1));
    cache.MarkReady(KeyFor(2));

    for (uint64 frame = 2; frame <= 5; ++frame)
    {
        EXPECT_EQ(cache.Request(KeyFor(3), frame, evicted), ImpostorAtlasCache::State::Absent);
        EXPECT_EQ(cache.Request(KeyFor(2), frame, evicted), ImpostorAtlasCache::State::Ready);
        EXPECT_EQ(cache.Request(KeyFor(1), frame, evicted), ImpostorAtlasCache::State::Ready);
    }
    EXPECT_TRUE(evicted.empty());

    EXPECT_EQ(cache.Request(KeyFor(2), 6, evicted), ImpostorAtlasCache::State::Ready);
    EXPECT_EQ(cache.Request(KeyFor(3), 7, evicted), ImpostorAtlasCache::State::Pending);
    EXPECT_EQ(evicted, std::vector<ImpostorAtlasKey>{KeyFor(1)});
    EXPECT_EQ(cache.GetUsedBytes(), cache.GetBudgetBytes());
}

TEST(ImpostorAtlasCache, AFailedBakeReturnsItsReservationWithoutEvictingReadyAtlases)
{
    const uint64 atlasBytes = ImpostorAtlasBytes(KeyFor(1));
    ImpostorAtlasCache cache(atlasBytes * 2);
    std::vector<ImpostorAtlasKey> evicted;
    ASSERT_EQ(cache.Request(KeyFor(1), 1, evicted), ImpostorAtlasCache::State::Pending);
    ASSERT_EQ(cache.Request(KeyFor(2), 1, evicted), ImpostorAtlasCache::State::Pending);
    cache.MarkReady(KeyFor(1));

    cache.ReleasePending(KeyFor(1));
    cache.ReleasePending(KeyFor(2));
    cache.ReleasePending(KeyFor(2));
    EXPECT_EQ(cache.GetState(KeyFor(1)), ImpostorAtlasCache::State::Ready);
    EXPECT_EQ(cache.GetState(KeyFor(2)), ImpostorAtlasCache::State::Absent);
    EXPECT_EQ(cache.GetUsedBytes(), atlasBytes);
    EXPECT_EQ(cache.Request(KeyFor(3), 1, evicted), ImpostorAtlasCache::State::Pending);
    EXPECT_TRUE(evicted.empty());
}

TEST(ImpostorAtlasCache, AnAtlasLargerThanTheBudgetIsRefused)
{
    ImpostorAtlasCache cache(ImpostorAtlasBytes(KeyFor(1)) - 1);
    std::vector<ImpostorAtlasKey> evicted;
    EXPECT_EQ(cache.Request(KeyFor(1), 1, evicted), ImpostorAtlasCache::State::Absent);
    EXPECT_EQ(cache.GetUsedBytes(), 0u);
}
