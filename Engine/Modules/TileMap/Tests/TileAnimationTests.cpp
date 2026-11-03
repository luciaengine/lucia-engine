#include "TileMap/TileAnimation.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
using namespace GameEngine::TileMap;

TEST(TileAnimationTests, TotalDurationSumsPositiveFrames)
{
    const std::vector<TileAnimationFrame> frames = {{1, 0.25f}, {2, 0.5f}, {3, -1.0f}};
    EXPECT_FLOAT_EQ(TotalAnimationDuration(frames), 0.75f);
    EXPECT_FLOAT_EQ(TotalAnimationDuration({}), 0.0f);
}

TEST(TileAnimationTests, SelectsFrameByElapsedTime)
{
    const std::vector<TileAnimationFrame> frames = {{1, 0.25f}, {2, 0.25f}, {3, 0.5f}};
    EXPECT_EQ(SelectAnimationFrame(frames, 0.0f), 0u);
    EXPECT_EQ(SelectAnimationFrame(frames, 0.24f), 0u);
    EXPECT_EQ(SelectAnimationFrame(frames, 0.25f), 1u);
    EXPECT_EQ(SelectAnimationFrame(frames, 0.49f), 1u);
    EXPECT_EQ(SelectAnimationFrame(frames, 0.5f), 2u);
    EXPECT_EQ(SelectAnimationFrame(frames, 0.99f), 2u);
}

TEST(TileAnimationTests, LoopsAndStaysInPhaseAcrossCycles)
{
    const std::vector<TileAnimationFrame> frames = {{1, 0.25f}, {2, 0.75f}};
    // Same phase, many cycles later — the global-clock guarantee.
    EXPECT_EQ(SelectAnimationFrame(frames, 0.1f), 0u);
    EXPECT_EQ(SelectAnimationFrame(frames, 100.1f), 0u);
    EXPECT_EQ(SelectAnimationFrame(frames, 0.6f), 1u);
    EXPECT_EQ(SelectAnimationFrame(frames, 100.6f), 1u);
}

TEST(TileAnimationTests, DegenerateInputsFallBackToFrameZero)
{
    EXPECT_EQ(SelectAnimationFrame({}, 1.0f), 0u);
    const std::vector<TileAnimationFrame> zeroLength = {{1, 0.0f}, {2, 0.0f}};
    EXPECT_EQ(SelectAnimationFrame(zeroLength, 1.0f), 0u);
    // Zero-duration frames inside a valid loop are skipped over.
    const std::vector<TileAnimationFrame> mixed = {{1, 0.0f}, {2, 0.5f}};
    EXPECT_EQ(SelectAnimationFrame(mixed, 0.25f), 1u);
    // Negative time (clock underflow) still lands inside the loop.
    const std::vector<TileAnimationFrame> frames = {{1, 0.5f}, {2, 0.5f}};
    EXPECT_EQ(SelectAnimationFrame(frames, -0.25f), 1u);
}
