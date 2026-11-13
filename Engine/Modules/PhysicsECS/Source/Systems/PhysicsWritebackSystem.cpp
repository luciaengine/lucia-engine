#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"

#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/KinematicBody.h"
#include "PhysicsECS/Components/MovingPlatform.h"

#include "Components/Transform.h"
#include "ECS/Query.h"

namespace GameEngine::PhysicsECS
{
void PhysicsWritebackSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    const float32 alpha = world.GetSimulationClock().GetAlpha();

    auto q = world.Query<
        ECS::Write<GameEngine::Components::Transform>,
        ECS::Write<GameEngine::Components::WorldTransform>,
        ECS::Write<GameEngine::Components::PhysicsBody>>();

    uint32 composed = 0;
    q.Each([&](ECS::EntityHandle e,
               ECS::Writable<GameEngine::Components::Transform> tWrite,
               ECS::Writable<GameEngine::Components::WorldTransform> wtWrite,
               ECS::Writable<GameEngine::Components::PhysicsBody> bodyWrite) {
        if (world.HasComponent<GameEngine::Components::KinematicBody>(e) &&
            !world.HasComponent<GameEngine::Components::MovingPlatform>(e))
            return;
        const auto& body = bodyWrite.Get();
        if (!body.initialized || !pw->IsBodyValid(body.body))
            return;

        // Interpolate from the snapshot captured in PhysicsStepSystem.
        const Physics::Transform* interpolateFrom =
            (body.hasPrevPhysicsTransform && alpha < 1.0f) ? &body.prevPhysicsTransform : nullptr;

        if (WritePhysicsPose(world, e, pw->GetBodyTransform(body.body), interpolateFrom, alpha, tWrite, wtWrite,
                             bodyWrite.Mutable().WritebackRecord))
            ++composed;
    });
    PhysicsWorldService::SetLastBodiesComposed(composed);
}

} // namespace GameEngine::PhysicsECS
