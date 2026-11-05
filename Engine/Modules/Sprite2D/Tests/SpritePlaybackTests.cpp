#include "Sprite2D/SpritePlayback.h"

#include <cmath>
#include <limits>
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
        t = AdvanceSpritePlayback(animation, t, 0.1f, 1.0f).TimeSeconds;

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
    EXPECT_FLOAT_EQ(AdvanceSpritePlayback(animation, 0.0f, 0.1f, 1.0f).TimeSeconds, 0.1f);
    EXPECT_FLOAT_EQ(AdvanceSpritePlayback(animation, 0.0f, 0.1f, 2.0f).TimeSeconds, 0.2f);
    EXPECT_FLOAT_EQ(AdvanceSpritePlayback(animation, 0.0f, 0.1f, 0.0f).TimeSeconds, 0.0f);
}

TEST(SpritePlayback, NegativeSpeedPlaysBackwards)
{
    const SpriteFrameAnimation animation = MakeAnimation(true);
    const float32 t = AdvanceSpritePlayback(animation, 0.05f, 0.1f, -1.0f).TimeSeconds;
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

    EXPECT_FLOAT_EQ(AdvanceSpritePlayback(animation, 0.0f, 1.0f, 1.0f).TimeSeconds, 0.0f);
    EXPECT_EQ(SpriteFrameIndexAt(animation, 1.0f), 0u);
}

TEST(SpritePlayback, OneShotCompletionReportsFinalFrameAndStopsAtEitherEnd)
{
    const auto animation = MakeAnimation(false);
    const auto forward = AdvanceSpritePlayback(animation, 0, 1, 1);
    EXPECT_EQ(forward.FrameIndex, 2u);
    EXPECT_EQ(forward.Events, 5u);
    EXPECT_EQ(forward.LoopCount, 0);
    const auto reverse = AdvanceSpritePlayback(animation, 0.15f, 1, -1);
    EXPECT_EQ(reverse.FrameIndex, 0u);
    EXPECT_EQ(reverse.Events, 5u);
    EXPECT_FLOAT_EQ(reverse.TimeSeconds, 0);
}

TEST(SpritePlayback, LargeLoopStepsReportSignedCrossingsAndTheFinalFrame)
{
    const auto animation = MakeAnimation(true);
    const auto forward = AdvanceSpritePlayback(animation, 0.05f, 1, 1);
    EXPECT_EQ(forward.Events, 3u);
    EXPECT_EQ(forward.LoopCount, 3);
    EXPECT_EQ(forward.FrameIndex, 1u);
    const auto reverse = AdvanceSpritePlayback(animation, 0.05f, 1, -1);
    EXPECT_EQ(reverse.Events, 3u);
    EXPECT_EQ(reverse.LoopCount, -4);
    EXPECT_EQ(reverse.FrameIndex, 2u);
}

TEST(SpritePlayback, WholeLoopsNotifyEvenWhenTheFinalFrameIsUnchanged)
{
    const auto animation = MakeAnimation(true);
    const auto duration = SpriteAnimationDuration(animation);
    for (const float32 speed : {-1.0f, 1.0f})
    {
        const auto step = AdvanceSpritePlayback(animation, 0, duration, speed);
        EXPECT_EQ(step.FrameIndex, 0u);
        EXPECT_EQ(step.Events, 3u);
        EXPECT_EQ(step.LoopCount, static_cast<int64>(speed));
    }
}

TEST(SpritePlayback, FrameChangesDoNotRequireLoopingOrCompletion)
{
    const auto animation = MakeAnimation(false);
    EXPECT_EQ(AdvanceSpritePlayback(animation, 0, 0.15f, 1).Events, 1u);
    EXPECT_EQ(AdvanceSpritePlayback(animation, 0, 0.05f, 1).Events, 0u);
    EXPECT_EQ(AdvanceSpritePlayback(animation, 0.1f, 0.1f, 0).Events, 0u);
}

TEST(SpritePlayback, NonFiniteInputsDoNotProduceEventsOrInvalidClocks)
{
    const auto animation = MakeAnimation(true);
    const auto infinity = std::numeric_limits<float32>::infinity();
    const auto invalid = std::numeric_limits<float32>::quiet_NaN();
    EXPECT_FLOAT_EQ(AdvanceSpritePlayback(animation, 0.05f, infinity, 1).TimeSeconds, 0.05f);
    EXPECT_FLOAT_EQ(AdvanceSpritePlayback(animation, 0.05f, 1, invalid).TimeSeconds, 0.05f);
    const auto step = AdvanceSpritePlayback(animation, invalid, 1, 1);
    EXPECT_FLOAT_EQ(step.TimeSeconds, 0);
    EXPECT_EQ(step.Events, 0u);
}

TEST(SpritePlayback, ExtremeLoopStepsSaturateCountsAndKeepTheClockFinite)
{
    const auto animation = MakeAnimation(true);
    const auto maximum = std::numeric_limits<float32>::max();
    const auto step = AdvanceSpritePlayback(animation, 0, maximum, maximum);
    EXPECT_TRUE(std::isfinite(step.TimeSeconds));
    EXPECT_GE(step.TimeSeconds, 0);
    EXPECT_LT(step.TimeSeconds, SpriteAnimationDuration(animation));
    EXPECT_EQ(step.LoopCount, std::numeric_limits<int64>::max());
    EXPECT_EQ(step.Events, 3u);
}

TEST(SpritePlayback, LargeWholeLoopsPreserveNonzeroPhaseInBothDirections)
{
    auto animation = MakeAnimation(true);
    animation.Frames.resize(2);
    for (auto& frame : animation.Frames) frame.DurationSeconds = 0.5f;
    for (const float32 speed : {-1.0f, 1.0f})
    {
        const auto step = AdvanceSpritePlayback(animation, 0.75f, std::ldexp(1.0f, 55), speed);
        EXPECT_FLOAT_EQ(step.TimeSeconds, 0.75f);
        EXPECT_EQ(step.FrameIndex, 1u);
        EXPECT_EQ(step.LoopCount, (int64{1} << 55) * static_cast<int64>(speed));
    }
}

TEST(SpritePlayback, LargeFractionalLoopsPreserveResidualCarryAndExactCounts)
{
    auto animation = MakeAnimation(true);
    animation.Frames.resize(2);
    for (auto& frame : animation.Frames) frame.DurationSeconds = 0.75f;
    const auto step = AdvanceSpritePlayback(animation, 1.25f, std::ldexp(1.0f, 63), 1);
    constexpr int64 kExpectedLoops = 6'148'914'691'236'517'206;
    EXPECT_EQ(step.LoopCount, kExpectedLoops);
    EXPECT_FLOAT_EQ(step.TimeSeconds, 0.25f);
}
