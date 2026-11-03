#include "PhysicsECS/Systems/PlatformPathTraversal.h"

#include <cmath>

namespace GameEngine::PhysicsECS
{
using Components::MovingPlatform;
using Components::PlatformPathMode;

namespace
{
// Reduce complete round trips before reflecting, independent of step length.
// At an exact terminus retain the incoming direction until the next step.
void ReflectIntoRange(MovingPlatform& platform, double distance, double pathLength)
{
    const double phase = std::fmod(std::abs(distance), 2.0 * pathLength);
    platform.distance = static_cast<float32>(phase <= pathLength ? phase : 2.0 * pathLength - phase);
    platform.direction = phase > 0.0 && phase <= pathLength ? 1 : -1;
}
} // namespace

bool AdvanceAlongPath(MovingPlatform& platform, float32 pathLength, float32 deltaTime)
{
    if (!std::isfinite(pathLength) || pathLength <= 0.0f ||
        !std::isfinite(deltaTime) || deltaTime <= 0.0f ||
        !std::isfinite(platform.speed) || platform.speed < 0.0f ||
        !std::isfinite(platform.endpointPause) || platform.endpointPause < 0.0f ||
        !std::isfinite(platform.distance) || !std::isfinite(platform.pauseRemaining) ||
        platform.pauseRemaining < 0.0f || (platform.direction != 1 && platform.direction != -1) ||
        (platform.pathMode != PlatformPathMode::Once && platform.pathMode != PlatformPathMode::Loop &&
         platform.pathMode != PlatformPathMode::PingPong))
        return false;
    if (platform.finished)
        return true;

    // Serve the endpoint hold first, and carry any leftover into motion so the
    // platform resumes at full speed instead of losing part of a frame.
    if (platform.pauseRemaining > 0.0f)
    {
        platform.pauseRemaining -= deltaTime;
        if (platform.pauseRemaining > 0.0f)
            return true;
        deltaTime = -platform.pauseRemaining;
        platform.pauseRemaining = 0.0f;
    }

    // A looping platform parked on the seam by a hold restarts from the top.
    if (platform.pathMode == PlatformPathMode::Loop)
    {
        if (platform.direction >= 0 && platform.distance >= pathLength)
            platform.distance = 0.0f;
        else if (platform.direction < 0 && platform.distance <= 0.0f)
            platform.distance = pathLength;
    }

    // Float inputs can produce a step larger than float's range; reduce it in
    // double precision before storing the bounded distance.
    const double distance = static_cast<double>(platform.distance) +
        static_cast<double>(platform.speed) * deltaTime * platform.direction;
    const bool pastEnd = distance > pathLength;
    const bool pastStart = distance < 0.0;
    if (!pastEnd && !pastStart)
    {
        platform.distance = static_cast<float32>(distance);
        return true;
    }

    const bool holds = platform.endpointPause > 0.0f;

    switch (platform.pathMode)
    {
    case PlatformPathMode::Once:
        // The far end is whichever terminus the platform was heading for.
        platform.distance = pastEnd ? pathLength : 0.0f;
        platform.finished = true;
        break;

    case PlatformPathMode::Loop:
        if (holds)
        {
            // Land on the seam and hold; the restart happens on resume above.
            platform.distance = pastEnd ? pathLength : 0.0f;
            platform.pauseRemaining = platform.endpointPause;
        }
        else
        {
            double wrapped = std::fmod(distance, static_cast<double>(pathLength));
            if (wrapped < 0.0)
                wrapped += pathLength;
            platform.distance = static_cast<float32>(wrapped);
        }
        break;

    case PlatformPathMode::PingPong:
        if (holds)
        {
            platform.distance = pastEnd ? pathLength : 0.0f;
            platform.direction = pastEnd ? -1 : 1;
            platform.pauseRemaining = platform.endpointPause;
        }
        else
        {
            ReflectIntoRange(platform, distance, pathLength);
        }
        break;
    }
    return true;
}

} // namespace GameEngine::PhysicsECS
