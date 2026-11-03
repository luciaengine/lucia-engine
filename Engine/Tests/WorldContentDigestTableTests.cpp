// WorldContentDigestTable: the per-world digests a shared render feature
// answers IRenderFeature::ContentDigest from. These pin the entry lifetime —
// an entry lives exactly as long as a Track subscription for its world — and
// that worlds never read each other's digest.

#include <gtest/gtest.h>

#include "Engine/Rendering/WorldContentDigestTable.h"

#include <memory>

using GameEngine::ScopedSubscription;
using GameEngine::Engine::Renderer::WorldContentDigestTable;

TEST(WorldContentDigestTableTest, UntrackedWorldReadsZero)
{
    WorldContentDigestTable table;
    EXPECT_EQ(table.Get(7u), 0u);
}

TEST(WorldContentDigestTableTest, WorldsKeepSeparateDigests)
{
    WorldContentDigestTable table;
    ScopedSubscription first = table.Track(1u);
    ScopedSubscription second = table.Track(2u);
    EXPECT_EQ(table.Get(1u), 0u);

    table.Set(1u, 11u);
    table.Set(2u, 22u);
    EXPECT_EQ(table.Get(1u), 11u);
    EXPECT_EQ(table.Get(2u), 22u);
}

TEST(WorldContentDigestTableTest, ReleasingTheSubscriptionDropsTheEntry)
{
    WorldContentDigestTable table;
    {
        ScopedSubscription tracking = table.Track(3u);
        table.Set(3u, 33u);
        EXPECT_EQ(table.Get(3u), 33u);
    }
    EXPECT_EQ(table.Get(3u), 0u);
}

// A system replaced on a plugin reload tracks its world before the old
// instance releases it: the entry must survive the old release.
TEST(WorldContentDigestTableTest, EntryLivesUntilTheLastSubscriptionIsReleased)
{
    WorldContentDigestTable table;
    ScopedSubscription oldSystem = table.Track(4u);
    ScopedSubscription newSystem = table.Track(4u);
    table.Set(4u, 44u);

    oldSystem.Reset();
    EXPECT_EQ(table.Get(4u), 44u);

    newSystem.Reset();
    EXPECT_EQ(table.Get(4u), 0u);
}

// A world (and its system) can outlive the render feature at shutdown.
TEST(WorldContentDigestTableTest, SubscriptionOutlivingTheTableReleasesSafely)
{
    auto table = std::make_unique<WorldContentDigestTable>();
    ScopedSubscription tracking = table->Track(5u);
    table.reset();
    tracking.Reset();
    EXPECT_FALSE(static_cast<bool>(tracking));
}
