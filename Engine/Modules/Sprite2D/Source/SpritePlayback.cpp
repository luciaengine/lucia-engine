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
    step.FrameIndex = SpriteFrameIndexAt(animation, step.TimeSeconds);
    if (animation.Frames.empty() || !std::isfinite(timeSeconds) || !std::isfinite(duration) ||
        !std::isfinite(deltaSeconds) || !std::isfinite(speedScale))
        return step;
    const float64 movement = static_cast<float64>(deltaSeconds) * speedScale;
    if (movement == 0)
        return step;
    const float64 destination = step.TimeSeconds + movement;
    const auto previousFrame = step.FrameIndex;
    step.TimeSeconds = NormalizeTime(destination, duration, animation.Loop);
    step.FrameIndex = SpriteFrameIndexAt(animation, step.TimeSeconds);
    if (animation.Loop && duration > 0)
    {
        const float64 loops = std::floor(destination / duration);
        if (loops >= static_cast<float64>(std::numeric_limits<int64>::max()))
            step.LoopCount = std::numeric_limits<int64>::max();
        else if (loops <= static_cast<float64>(std::numeric_limits<int64>::min()))
            step.LoopCount = std::numeric_limits<int64>::min();
        else
            step.LoopCount = static_cast<int64>(loops);
        if (step.LoopCount != 0)
            step.Events |= static_cast<uint32>(SpritePlaybackEvent::Looped);
    }
    else if (!animation.Loop && ((movement > 0 && destination >= duration) ||
                                (movement < 0 && destination <= 0)))
        step.Events |= static_cast<uint32>(SpritePlaybackEvent::Finished);
    if (step.FrameIndex != previousFrame || step.LoopCount != 0)
        step.Events |= static_cast<uint32>(SpritePlaybackEvent::FrameChanged);
    return step;
}

std::size_t SpriteFrameIndexAt(const SpriteFrameAnimation& animation, float32 timeSeconds)
{
    if (animation.Frames.empty())
        return kInvalidSpriteFrameIndex;

    const float32 t = NormalizeSpriteTime(animation, timeSeconds);

    float32 cursor = 0.0f;
    for (std::size_t i = 0; i < animation.Frames.size(); ++i)
    {
        cursor += std::max(0.0f, animation.Frames[i].DurationSeconds);
        if (t <= cursor)
            return i;
    }
    return animation.Frames.size() - 1;
}

} // namespace Sprite2D
} // namespace GameEngine
