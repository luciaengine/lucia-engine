#include "Editor/Entities/SpritePlacementSize.h"

#include <gtest/gtest.h>
#include <limits>

using GameEngine::Editor::ResolveSpritePlacementSize;

TEST(SpritePlacementSize, MatchesTileWorldDimensions)
{
    // A 32-pixel tree on a 16-pixel-per-unit tile grid spans two cells.
    const auto size = ResolveSpritePlacementSize(32, 64, 16);
    EXPECT_FLOAT_EQ(size.Width, 2);
    EXPECT_FLOAT_EQ(size.Height, 4);
}

TEST(SpritePlacementSize, UnknownDensityPreservesLegacyDrops)
{
    const auto size = ResolveSpritePlacementSize(32, 64, 0);
    EXPECT_FLOAT_EQ(size.Width, 32);
    EXPECT_FLOAT_EQ(size.Height, 64);
}

TEST(SpritePlacementSize, RejectsNonFiniteAndNonPositiveDensity)
{
    for (float bad : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()})
    {
        EXPECT_FLOAT_EQ(ResolveSpritePlacementSize(32, 64, bad).Width, 32);
    }
}

TEST(SpritePlacementSize, InvalidDimensionsCannotPublishInvalidTransforms)
{
    for (float bad : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()})
    {
        EXPECT_FLOAT_EQ(ResolveSpritePlacementSize(bad, 64, 16).Width, 1);
        EXPECT_FLOAT_EQ(ResolveSpritePlacementSize(32, bad, 16).Height, 1);
    }
    EXPECT_FLOAT_EQ(ResolveSpritePlacementSize(32, 64,
        std::numeric_limits<float>::denorm_min()).Height, 1);
}

TEST(SpritePlacementSize, LargeArtworkUsesTheSameSceneDensity)
{
    const auto size = ResolveSpritePlacementSize(1024, 512, 16);
    EXPECT_FLOAT_EQ(size.Width, 64);
    EXPECT_FLOAT_EQ(size.Height, 32);
}
