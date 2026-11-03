#include "Sprite2D/SpritePlayback.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Sprite2D;

namespace
{

// Three 0.1s frames: [0,0.1] A, (0.1,0.2] B, (0.2,0.3] C.
SpriteFrameAnimation MakeAnimation(bool loop)
{
    SpriteFrameAnimation animation;
    animation.Name = "Run";
    animation.Loop = loop;
    for (const char* name : {"A", "B", "C"})
    {
        SpriteFrame frame;
        frame.Name = name;
        frame.DurationSeconds = 0.1f;
        animation.Frames.push_back(frame);
    }
    return animation;
}

} // namespace

TEST(SpritePlayback, DurationSumsFrames)
{
    EXPECT_FLOAT_EQ(SpriteAnimationDuration(MakeAnimation(true)), 0.3f);
    EXPECT_FLOAT_EQ(SpriteAnimationDuration(SpriteFrameAnimation{}), 0.0f);
}

TEST(SpritePlayback, FrameBoundariesBelongToTheEarlierFrame)
{
    const SpriteFrameAnimation animation = MakeAnimation(true);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 0.0f), 0u);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 0.05f), 0u);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 0.1f), 0u);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 0.15f), 1u);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 0.25f), 2u);
}

TEST(SpritePlayback, EmptyAnimationHasNoFrame)
{
    EXPECT_EQ(SpriteFrameIndexAt(SpriteFrameAnimation{}, 0.5f), kInvalidSpriteFrameIndex);
}

TEST(SpritePlayback, LoopingWrapsPastTheEnd)
{
    const SpriteFrameAnimation animation = MakeAnimation(true);
    // NEAR, not FLOAT_EQ: the duration is a running sum of 0.1f, so the wrap
    // inherits that sum's representation error rather than landing on 0.05 exactly.
    EXPECT_NEAR(NormalizeSpriteTime(animation, 0.35f), 0.05f, 1.0e-6f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 0.35f), 0u);
}

// The reason Advance normalizes rather than letting the component accumulate:
// after thousands of steps the clock is still small, so frame selection keeps
// full float precision instead of degrading with uptime.
TEST(SpritePlayback, RepeatedAdvanceKeepsTheClockInRange)
{
    const SpriteFrameAnimation animation = MakeAnimation(true);
    float32 t = 0.0f;
    for (int32 i = 0; i < 10000; ++i)
        t = AdvanceSpriteTime(animation, t, 0.1f, 1.0f);

    EXPECT_GE(t, 0.0f);
    EXPECT_LE(t, SpriteAnimationDuration(animation));
    EXPECT_NEAR(t, 0.1f, 1.0e-3f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, t), 0u);
}

TEST(SpritePlayback, LoopingWrapsNegativeTimeForward)
{
    const SpriteFrameAnimation animation = MakeAnimation(true);
    EXPECT_NEAR(NormalizeSpriteTime(animation, -0.05f), 0.25f, 1.0e-6f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, -0.05f), 2u);
}

TEST(SpritePlayback, OneShotClampsAndHoldsTheLastFrame)
{
    const SpriteFrameAnimation animation = MakeAnimation(false);
    EXPECT_NEAR(NormalizeSpriteTime(animation, 5.0f), 0.3f, 1.0e-6f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 5.0f), 2u);
    EXPECT_FLOAT_EQ(NormalizeSpriteTime(animation, -1.0f), 0.0f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, -1.0f), 0u);
}

TEST(SpritePlayback, AdvanceScalesDeltaBySpeed)
{
    const SpriteFrameAnimation animation = MakeAnimation(true);
    EXPECT_FLOAT_EQ(AdvanceSpriteTime(animation, 0.0f, 0.1f, 1.0f), 0.1f);
    EXPECT_FLOAT_EQ(AdvanceSpriteTime(animation, 0.0f, 0.1f, 2.0f), 0.2f);
    EXPECT_FLOAT_EQ(AdvanceSpriteTime(animation, 0.0f, 0.1f, 0.0f), 0.0f);
}

TEST(SpritePlayback, NegativeSpeedPlaysBackwards)
{
    const SpriteFrameAnimation animation = MakeAnimation(true);
    const float32 t = AdvanceSpriteTime(animation, 0.05f, 0.1f, -1.0f);
    EXPECT_FLOAT_EQ(t, 0.25f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, t), 2u);
}

TEST(SpritePlayback, ZeroLengthAnimationStaysOnItsFirstFrame)
{
    SpriteFrameAnimation animation;
    animation.Loop = true;
    SpriteFrame frame;
    frame.DurationSeconds = 0.0f;
    animation.Frames.push_back(frame);

    EXPECT_FLOAT_EQ(AdvanceSpriteTime(animation, 0.0f, 1.0f, 1.0f), 0.0f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 1.0f), 0u);
}
