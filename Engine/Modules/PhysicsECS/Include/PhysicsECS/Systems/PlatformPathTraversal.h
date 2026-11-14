#pragma once

#include "PhysicsECS/Components/MovingPlatform.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
// Advance a platform's arc-length position along a path of `pathLength` metres
// by `deltaTime` seconds, applying the loop/ping-pong/once rule and the endpoint
// hold. Updates `platform.distance`, `direction`, `pauseRemaining` and
// `finished`; touches nothing else.
//
// Kept free of ECS, splines and physics so the traversal rules can be exercised
// on their own — the geometry a platform follows has no bearing on when it turns
// around.
//
// A terminus is never overshot when `endpointPause` is set: the platform lands
// exactly on the end, holds there, and only then continues. Without a pause it
// carries the leftover travel through the turn, so a large step cannot stall it.
// Returns false without changing state for nonfinite or invalid traversal inputs.
bool AdvanceAlongPath(Components::MovingPlatform& platform, float32 pathLength, float32 deltaTime);

} // namespace GameEngine::PhysicsECS
