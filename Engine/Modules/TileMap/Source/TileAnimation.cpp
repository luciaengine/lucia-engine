#include "TileMap/TileAnimation.h"

#include <cmath>

namespace GameEngine
{
namespace TileMap
{

float32 TotalAnimationDuration(std::span<const TileAnimationFrame> frames)
{
    float32 total = 0.0f;
    for (const TileAnimationFrame& frame : frames)
        total += std::max(0.0f, frame.DurationSeconds);
    return total;
}

size_t SelectAnimationFrame(std::span<const TileAnimationFrame> frames, float32 timeSeconds)
{
    const float32 total = TotalAnimationDuration(frames);
    if (total <= 0.0f || frames.empty())
        return 0;

    float32 t = std::fmod(timeSeconds, total);
    if (t < 0.0f)
        t += total;

    float32 end = 0.0f;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        end += std::max(0.0f, frames[i].DurationSeconds);
        if (t < end)
            return i;
    }
    return frames.size() - 1;
}

} // namespace TileMap
} // namespace GameEngine
