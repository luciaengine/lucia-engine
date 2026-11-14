#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{
// What happens when the platform reaches the end of its path.
enum class PlatformPathMode : uint8
{
    Once,     // Stop at the far end and stay there.
    Loop,     // Jump back to the start and run the path again.
    PingPong, // Reverse direction and run the path backwards.
};

// Drives a kinematic PhysicsBody along a spline path.
//
// The path lives on a SEPARATE entity carrying a SplineComponent: a spline's
// control points are stored in its owner's local space, so a path parented to
// the platform would move with the platform it is supposed to steer. Keeping it
// on its own (stationary) entity also makes it authorable and visible in the
// editor with the existing spline tool.
//
// Requires a PhysicsBody with motionType == Kinematic on the same entity. The
// system moves it via PhysicsWorld::MoveKinematic — never by writing the
// transform — so contact friction carries dynamic riders for free.
struct MovingPlatform
{
    PlatformPathMode pathMode = PlatformPathMode::PingPong;

    // +1 travels the path from start to end, -1 from end to start. PingPong
    // flips it at each terminus; the other modes leave it alone.
    int8 direction = 1;

    // Set once the platform has stopped for good (Once mode, far end reached).
    bool finished = false;

    // Entity holding the SplineComponent that describes the path.
    ECS::EntityHandle path{};

    // Metres per second along the path's arc length, so a curve and a straight
    // line of the same speed take the same time per metre.
    float32 speed = 2.0f;

    // Seconds to hold still on arriving at a terminus. For Loop this is the
    // pause at the seam before restarting; for PingPong it applies at both ends.
    float32 endpointPause = 0.0f;

    // Runtime: arc length travelled from the start of the path, and the
    // remaining hold at the terminus just reached.
    float32 distance = 0.0f;
    float32 pauseRemaining = 0.0f;
};

static_assert(std::is_trivially_copyable_v<MovingPlatform>,
              "MovingPlatform must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<MovingPlatform>,
              "MovingPlatform must be standard layout for ECS storage");

} // namespace GameEngine::Components
