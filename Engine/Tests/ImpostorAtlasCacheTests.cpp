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

TEST(ImpostorAtlasCache, AnAtlasOfEightByEightFramesAt128PixelsIsTwelveMegabytes)
{
    // 64 frames x 128^2 pixels x (4 + 8) bytes.
    EXPECT_EQ(ImpostorAtlasBytes(KeyFor(1)), 64ull * 128 * 128 * 12);
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

TEST(ImpostorAtlasCache, TouchingAnAbsentAtlasKeepsReservationsUntilAdmission)
{
    using State = ImpostorAtlasCache::State;
    const ImpostorAtlasKey resident = KeyFor(1);
    const ImpostorAtlasKey incoming = KeyFor(2);
    const uint64 budget = ImpostorAtlasBytes(resident);
    ImpostorAtlasCache cache(budget);
    std::vector<ImpostorAtlasKey> evicted;

    ASSERT_EQ(cache.Request(resident, 1, evicted), State::Pending);
    cache.MarkReady(resident);

    EXPECT_EQ(cache.Touch(incoming, 2), State::Absent);
    EXPECT_EQ(cache.GetUsedBytes(), budget);
    EXPECT_EQ(cache.Touch(resident, 2), State::Ready);
    EXPECT_EQ(cache.Request(incoming, 2, evicted), State::Absent);
    EXPECT_TRUE(evicted.empty());

    EXPECT_EQ(cache.Request(incoming, 3, evicted), State::Pending);
    EXPECT_EQ(evicted, std::vector<ImpostorAtlasKey>{resident});
}

TEST(ImpostorAtlasCache, AnAtlasLargerThanTheBudgetIsRefused)
{
    ImpostorAtlasCache cache(ImpostorAtlasBytes(KeyFor(1)) - 1);
    std::vector<ImpostorAtlasKey> evicted;
    EXPECT_EQ(cache.Request(KeyFor(1), 1, evicted), ImpostorAtlasCache::State::Absent);
    EXPECT_EQ(cache.GetUsedBytes(), 0u);
}
