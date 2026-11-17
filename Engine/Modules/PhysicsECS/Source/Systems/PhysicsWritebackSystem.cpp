#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"

#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include "PhysicsECS/Components/KinematicBody.h"
#include "PhysicsECS/Components/MovingPlatform.h"
#include "PhysicsECS/Components/PhysicsBody.h"

#include "Components/Transform.h"
#include "ECS/Query.h"

namespace GameEngine::PhysicsECS
{
namespace
{
// Write the body pose of `e` into its Transform and WorldTransform. True when the pose changed.
bool ComposeBodyPose(ECS::World& world, Physics::PhysicsWorld& pw, float32 alpha, ECS::EntityHandle e,
                     const Components::Transform& t, const Components::WorldTransform& wt, Components::PhysicsBody& body)
{
    if (!body.initialized || !pw.IsBodyValid(body.body))
        return false;

    // Interpolate from the snapshot captured in PhysicsStepSystem.
    const Physics::Transform* interpolateFrom =
        (body.hasPrevPhysicsTransform && alpha < 1.0f) ? &body.prevPhysicsTransform : nullptr;
    const auto record = WritePhysicsPose(world, e, pw.GetBodyTransform(body.body), interpolateFrom, alpha, t, wt,
                                         body.WritebackRecord);
    if (!record)
        return false;
    body.WritebackRecord = *record;
    return true;
}
} // namespace

void PhysicsWritebackSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    const float32 alpha = world.GetSimulationClock().GetAlpha();
    uint32 composed = 0;
    const auto compose = [&world, pw, alpha, &composed](ECS::EntityHandle e, const Components::Transform& t,
                                                         const Components::WorldTransform& wt, Components::PhysicsBody& body)
    {
        if (ComposeBodyPose(world, *pw, alpha, e, t, wt, body))
            ++composed;
    };

    auto bodies = world.Query<
        ECS::Read<GameEngine::Components::Transform>,
        ECS::Read<GameEngine::Components::WorldTransform>,
        ECS::Write<GameEngine::Components::PhysicsBody>>();
    // A kinematic body follows its Transform (PhysicsStepSystem moves it there over the frame's steps), so
    // the Transform is the source and is not written back. Leaving its chunks out of the visit also leaves
    // their WorldTransform unstamped, so the step's change gate sees only the bodies that moved.
    bodies.Without<GameEngine::Components::KinematicBody>();
    bodies.Each(compose);

    // A moving platform is the exception: MovingPlatformSystem moves its kinematic body, and the body
    // pose is the source of its Transform.
    auto platforms = world.Query<
        ECS::Read<GameEngine::Components::Transform>,
        ECS::Read<GameEngine::Components::WorldTransform>,
        ECS::Write<GameEngine::Components::PhysicsBody>>();
    platforms.With<GameEngine::Components::KinematicBody, GameEngine::Components::MovingPlatform>();
    platforms.Each(compose);

    PhysicsWorldService::SetLastBodiesComposed(composed);
}

} // namespace GameEngine::PhysicsECS
