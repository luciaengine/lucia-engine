#include "Sprite2D/SpritePlayback.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace Sprite2D
{

float32 SpriteAnimationDuration(const SpriteFrameAnimation& animation)
{
    float32 duration = 0.0f;
    for (const auto& frame : animation.Frames)
        duration += std::max(0.0f, frame.DurationSeconds);
    return duration;
}

float32 NormalizeSpriteTime(const SpriteFrameAnimation& animation, float32 timeSeconds)
{
    const float32 duration = SpriteAnimationDuration(animation);
    if (duration <= 0.0f)
        return 0.0f;

    if (!animation.Loop)
        return std::clamp(timeSeconds, 0.0f, duration);

    float32 wrapped = std::fmod(timeSeconds, duration);
    if (wrapped < 0.0f)
        wrapped += duration;
    return wrapped;
}

float32 AdvanceSpriteTime(const SpriteFrameAnimation& animation, float32 timeSeconds,
                          float32 deltaSeconds, float32 speedScale)
{
    return NormalizeSpriteTime(animation, timeSeconds + deltaSeconds * speedScale);
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
