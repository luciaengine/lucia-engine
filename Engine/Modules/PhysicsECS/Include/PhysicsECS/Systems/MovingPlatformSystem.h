#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
// Drives each MovingPlatform's kinematic body along its spline path.
//
// Runs between PhysicsInit (which creates the body) and PhysicsStep (which
// integrates the velocity this system sets), so a platform's motion and the
// contacts that carry its riders are resolved in the same simulation step.
class MovingPlatformSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "MovingPlatformSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} // namespace GameEngine::PhysicsECS
