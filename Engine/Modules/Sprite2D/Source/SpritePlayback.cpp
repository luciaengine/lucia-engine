#include "Sprite2D/SpritePlayback.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine
{
namespace Sprite2D
{
namespace
{
float32 NormalizeTime(float64 timeSeconds, float32 duration, bool loop)
{
    if (!std::isfinite(timeSeconds) || !std::isfinite(duration) || duration <= 0.0f)
        return 0.0f;
    if (!loop)
        return static_cast<float32>(std::clamp(timeSeconds, 0.0, static_cast<float64>(duration)));
    float64 wrapped = std::fmod(timeSeconds, duration);
    if (wrapped < 0)
        wrapped += duration;
    return std::min(static_cast<float32>(wrapped), std::nextafter(duration, 0.0f));
}

std::size_t FrameIndexAtNormalizedTime(const SpriteFrameAnimation& animation, float32 timeSeconds)
{
    if (animation.Frames.empty())
        return kInvalidSpriteFrameIndex;
    float32 cursor = 0;
    for (std::size_t index = 0; index < animation.Frames.size(); ++index)
    {
        cursor += std::max(0.0f, animation.Frames[index].DurationSeconds);
        if (timeSeconds <= cursor)
            return index;
    }
    return animation.Frames.size() - 1;
}

bool ReachesUpperEndpoint(float64 phase, float64 movement, float64 duration)
{
    const float64 larger = std::max(phase, movement);
    const float64 smaller = std::min(phase, movement);
    // Subtraction from the larger addend is exact when a crossing is possible.
    return larger >= duration || (larger >= duration * 0.5 && smaller >= duration - larger);
}

int64 CountLoopCrossings(float64 movement, float32 duration, float64 remainder, int64 carry)
{
    if (std::abs(movement) < duration)
        return carry;
    constexpr int64 kLoopChunk = int64{1} << 32;
    constexpr float64 kLoopLimit = static_cast<float64>(uint64{1} << 63);
    const float64 estimate = movement / duration;
    if (estimate >= kLoopLimit + kLoopChunk)
        return std::numeric_limits<int64>::max();
    if (estimate <= -kLoopLimit - kLoopChunk)
        return std::numeric_limits<int64>::min();
    // Integer chunks keep all 64 count bits. The remainder corrects division rounding.
    const float64 high = std::clamp(std::trunc(estimate / kLoopChunk),
        -kLoopLimit / kLoopChunk, kLoopLimit / kLoopChunk - 1.0);
    const int64 base = static_cast<int64>(high) * kLoopChunk;
    const float64 residual = std::fma(-high, static_cast<float64>(duration) * kLoopChunk, movement);
    const int64 low = static_cast<int64>(std::round((residual - remainder) / duration)) + carry;
    if (low > 0 && base > std::numeric_limits<int64>::max() - low)
        return std::numeric_limits<int64>::max();
    if (low < 0 && base < std::numeric_limits<int64>::min() - low)
        return std::numeric_limits<int64>::min();
    return base + low;
}
}

float32 SpriteAnimationDuration(const SpriteFrameAnimation& animation)
{
    float32 duration = 0.0f;
    for (const auto& frame : animation.Frames)
        duration += std::max(0.0f, frame.DurationSeconds);
    return duration;
}

float32 NormalizeSpriteTime(const SpriteFrameAnimation& animation, float32 timeSeconds)
{
    return NormalizeTime(timeSeconds, SpriteAnimationDuration(animation), animation.Loop);
}

SpritePlaybackStep AdvanceSpritePlayback(const SpriteFrameAnimation& animation, float32 timeSeconds,
                                         float32 deltaSeconds, float32 speedScale)
{
    SpritePlaybackStep step;
    const float32 duration = SpriteAnimationDuration(animation);
    step.TimeSeconds = NormalizeTime(timeSeconds, duration, animation.Loop);
    step.FrameIndex = FrameIndexAtNormalizedTime(animation, step.TimeSeconds);
    if (animation.Frames.empty() || !std::isfinite(timeSeconds) || !std::isfinite(duration) ||
        !std::isfinite(deltaSeconds) || !std::isfinite(speedScale))
        return step;
    const float64 movement = static_cast<float64>(deltaSeconds) * speedScale;
    if (movement == 0)
        return step;
    const float64 destination = step.TimeSeconds + movement;
    const auto previousFrame = step.FrameIndex;
    if (animation.Loop && duration > 0)
    {
        const float64 remainder = std::fmod(movement, duration);
        float64 phase = step.TimeSeconds;
        int64 carry = 0;
        if (remainder > 0 && ReachesUpperEndpoint(phase, remainder, duration))
        {
            carry = 1;
            phase = std::min(phase, remainder) - (duration - std::max(phase, remainder));
        }
        else if (phase < -remainder)
        {
            carry = -1;
            phase = duration - (-remainder - phase);
        }
        else
            phase += remainder;
        step.TimeSeconds = std::min(static_cast<float32>(phase), std::nextafter(duration, 0.0f));
        step.LoopCount = CountLoopCrossings(movement, duration, remainder, carry);
        if (step.LoopCount != 0)
            step.Events |= static_cast<uint32>(SpritePlaybackEvent::Looped);
    }
    else
    {
        const bool finished = !animation.Loop && (movement > 0 ?
            ReachesUpperEndpoint(step.TimeSeconds, movement, duration) : -movement >= step.TimeSeconds);
        step.TimeSeconds = NormalizeTime(destination, duration, animation.Loop);
        if (finished)
            step.Events |= static_cast<uint32>(SpritePlaybackEvent::Finished);
        else if (!animation.Loop && movement > 0)
            step.TimeSeconds = std::min(step.TimeSeconds, std::nextafter(duration, 0.0f));
    }
    step.FrameIndex = FrameIndexAtNormalizedTime(animation, step.TimeSeconds);
    if (step.FrameIndex != previousFrame || step.LoopCount != 0)
        step.Events |= static_cast<uint32>(SpritePlaybackEvent::FrameChanged);
    return step;
}

std::size_t SpriteFrameIndexAt(const SpriteFrameAnimation& animation, float32 timeSeconds)
{
    return FrameIndexAtNormalizedTime(animation, NormalizeSpriteTime(animation, timeSeconds));
}

} // namespace Sprite2D
} // namespace GameEngine
