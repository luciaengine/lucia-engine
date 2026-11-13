#include "PhysicsECS/Systems/MovingPlatformSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/PlatformPathTraversal.h"

#include "PhysicsECS/Components/MovingPlatform.h"
#include "PhysicsECS/Components/PhysicsBody.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"

#include <cmath>
#include <cstring>

namespace GameEngine::PhysicsECS
{
namespace
{
void StopPlatform(Physics::PhysicsWorld& physics, Physics::BodyHandle body)
{
    const Physics::Vector3 velocity = physics.GetLinearVelocity(body);
    if (velocity.x != 0.0f || velocity.y != 0.0f || velocity.z != 0.0f)
        physics.SetLinearVelocity(body, Physics::Vector3{});
}

// Mean length of the world matrix's three basis vectors: the factor by which
// the owning entity's transform stretches the spline's local arc length.
// Exact for a uniformly scaled path; for a non-uniformly scaled one no single
// factor exists, and speed becomes an approximation that varies along the path.
float32 ArcLengthScale(const Components::WorldTransform& xf)
{
    const float32 sx = glm::length(xf.Basis[0]);
    const float32 sy = glm::length(xf.Basis[1]);
    const float32 sz = glm::length(xf.Basis[2]);
    return (sx + sy + sz) / 3.0f;
}
} // namespace

void MovingPlatformSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    auto* splines = SplineECS::SplineService::TryGet();
    auto q = world.Query<ECS::Write<GameEngine::Components::MovingPlatform>,
                         ECS::Read<GameEngine::Components::PhysicsBody>,
                         ECS::Optional<ECS::ComponentDisabled<GameEngine::Components::MovingPlatform>>>();
    q.IncludeDisabled<GameEngine::Components::MovingPlatform>();

    q.Each([&](ECS::EntityHandle /*e*/,
               ECS::Writable<GameEngine::Components::MovingPlatform> platformRow,
               const GameEngine::Components::PhysicsBody& body,
               const ECS::ComponentDisabled<GameEngine::Components::MovingPlatform>* disabled)
    {
                auto& platform = platformRow.Mutable();
        if (body.motionType != Physics::MotionType::Kinematic ||
            !body.initialized || !pw->IsBodyValid(body.body))
            return;

        if (disabled || !splines || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
        {
            StopPlatform(*pw, body.body);
            return;
        }

        const auto* pathSpline = world.GetComponent<GameEngine::Components::SplineComponent>(platform.path);
        const auto* pathXf = world.GetComponent<GameEngine::Components::WorldTransform>(platform.path);
        if (!pathSpline || !pathXf ||
            world.HasComponent<ECS::ComponentDisabled<GameEngine::Components::SplineComponent>>(platform.path) ||
            world.HasComponent<ECS::Disabled>(platform.path) ||
            world.HasComponent<ECS::DisabledInHierarchy>(platform.path))
        {
            StopPlatform(*pw, body.body);
            return;
        }

        const SplineECS::SplineHandle handle(pathSpline->SplineDataIndex, pathSpline->SplineDataGeneration);
        const Spline::SplineData* data = splines->GetSplineData(handle);
        if (!data || !data->IsValid())
        {
            StopPlatform(*pw, body.body);
            return;
        }

        // Every arc-length query answers 0 on an unbaked cache, which would read
        // as "the whole path is one point" and park the platform on the spline's
        // origin. Stop instead, and pick the platform up once
        // SplineExtractionSystem has rebuilt the cache.
        const float32 scale = ArcLengthScale(*pathXf);
        const float32 worldLength = data->TotalArcLength * scale;
        if (!AdvanceAlongPath(platform, worldLength, deltaTime))
        {
            StopPlatform(*pw, body.body);
            return;
        }

        // Back to the spline's own parameterisation before evaluating: the
        // control points, and therefore TotalArcLength, are entity-local.
        const float32 localDistance = (scale > 0.0f) ? (platform.distance / scale) : 0.0f;
        const Mathematics::Vector3 localPos = Spline::EvaluateAtDistance(*data, localDistance).Position;

        const Mathematics::WorldPosition worldPos = pathXf->Position + Mathematics::Vector3(pathXf->Basis.GetGLM() * localPos.GetGLM());

        if (!std::isfinite(worldPos.X) || !std::isfinite(worldPos.Y) || !std::isfinite(worldPos.Z))
        {
            StopPlatform(*pw, body.body);
            return;
        }

        // Position only: the platform keeps its authored orientation, so the
        // target rotation is the one it already has and the resulting angular
        // velocity is zero. Riders are carried by the surface translating under
        // them; spinning the deck would need the rider's own contact point to
        // pick up the tangential term, which GetPointVelocity exposes.
        Physics::Transform target = pw->GetBodyTransform(body.body);
        target.position = worldPos;

        pw->MoveKinematic(body.body, target, deltaTime);
    });
}

} // namespace GameEngine::PhysicsECS
