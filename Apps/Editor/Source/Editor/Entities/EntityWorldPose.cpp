#include "Editor/Entities/EntityWorldPose.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsWorldService.h"


namespace GameEngine::Editor
{

using Components::Parent;
using Components::PhysicsBody;
using Components::Transform;
using Components::WorldTransform;
using Mathematics::Matrix4x4;
using Mathematics::Quaternion;
using Mathematics::Vector3;
using Mathematics::WorldPosition;

namespace
{
// Guards the recursive parent walk against a cyclic Parent chain.
constexpr int kMaxHierarchyDepth = 64;

EntityTRS GetLocalTRS(ECS::Entity& e)
{
    EntityTRS t{};
    if (const Transform* xf = e.Get<Transform>())
    {
        t.Position = xf->GetPosition();
        t.Rotation = xf->GetRotation();
        t.Scale = xf->GetScale();
    }
    return t;
}

bool TryComputeHierarchyWorldPoseAtDepth(ECS::World* world, ECS::EntityHandle entity,
                                        WorldTransform& outPose, int depth)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity) || depth > kMaxHierarchyDepth)
        return false;
    if (const auto* local = world->GetComponent<Transform>(entity))
    {
        if (const auto* parent = world->GetComponent<Parent>(entity);
            parent && parent->Entity.IsValid() && world->IsValid(parent->Entity))
        {
            WorldTransform parentPose;
            if (!TryComputeHierarchyWorldPoseAtDepth(world, parent->Entity, parentPose, depth + 1))
                return false;
            Components::ComposeWorldPose(parentPose, *local, outPose);
        }
        else
        {
            outPose.Position = local->Position;
            outPose.Basis = local->Basis;
        }
        return true;
    }
    if (const auto* placed = world->GetComponent<WorldTransform>(entity))
    {
        outPose = *placed;
        return true;
    }
    return false;
}
} // namespace

EntityTRS CombineTRS(const EntityTRS& parent, const EntityTRS& local)
{
    EntityTRS out{};
    out.Scale = Vector3(
        parent.Scale.x * local.Scale.x,
        parent.Scale.y * local.Scale.y,
        parent.Scale.z * local.Scale.z);

    out.Rotation = parent.Rotation * local.Rotation;

    const Transform parentLocal = Transform::FromTRS(parent.Position, parent.Rotation, parent.Scale);
    WorldTransform parentPose;
    parentPose.Position = parentLocal.Position;
    parentPose.Basis = parentLocal.Basis;
    const Transform localPose = Transform::FromTRS(local.Position, local.Rotation, local.Scale);
    WorldTransform composed;
    Components::ComposeWorldPose(parentPose, localPose, composed);
    out.Position = composed.Position;

    return out;
}

EntityTRS ComputeWorldTRSForEntity(ECS::World* world, ECS::EntityHandle handle)
{
    if (!world || !handle.IsValid())
    {
        return EntityTRS{};
    }

    ECS::Entity e(world, handle);
    if (!e.IsValid())
    {
        return EntityTRS{};
    }

    const EntityTRS local = GetLocalTRS(e);

    if (const auto* parentComp = e.Get<Parent>())
    {
        ECS::Entity parentEntity(world, parentComp->Entity);
        if (parentEntity.IsValid())
        {
            return CombineTRS(ComputeWorldTRSForEntity(world, parentComp->Entity), local);
        }
    }

    return local;
}

void GetParentWorldTRS(ECS::World* world,
                       ECS::EntityHandle handle,
                       WorldPosition& outPos,
                       Quaternion& outRot,
                       Vector3& outScale)
{
    const EntityTRS identity{};
    outPos = identity.Position;
    outRot = identity.Rotation;
    outScale = identity.Scale;

    if (!world)
    {
        return;
    }

    ECS::Entity e(world, handle);
    if (!e.IsValid())
    {
        return;
    }

    const auto* parentComp = e.Get<Parent>();
    if (!parentComp)
    {
        return;
    }

    const EntityTRS parentWorld = ComputeWorldTRSForEntity(world, parentComp->Entity);
    outPos = parentWorld.Position;
    outRot = parentWorld.Rotation;
    outScale = parentWorld.Scale;
}

WorldPosition ComputeLocalPositionFromWorld(const WorldPosition& worldPos,
                                            const WorldPosition& parentPos,
                                            const Quaternion& parentRot,
                                            const Vector3& parentScale)
{
    Transform offset;
    offset.Position = WorldPosition{worldPos.X - parentPos.X, worldPos.Y - parentPos.Y, worldPos.Z - parentPos.Z};
    WorldTransform inverseRotation;
    inverseRotation.Basis = Transform::FromTRS(WorldPosition{}, parentRot.Conjugated(), Vector3{1.0f, 1.0f, 1.0f}).Basis;
    WorldTransform unrotated;
    Components::ComposeWorldPose(inverseRotation, offset, unrotated);
    return WorldPosition{
        parentScale.x != 0.0f ? unrotated.Position.X / parentScale.x : unrotated.Position.X,
        parentScale.y != 0.0f ? unrotated.Position.Y / parentScale.y : unrotated.Position.Y,
        parentScale.z != 0.0f ? unrotated.Position.Z / parentScale.z : unrotated.Position.Z};
}

bool TryComputeHierarchyWorldMatrix(ECS::World* world, ECS::EntityHandle entity, Matrix4x4& outMatrix)
{
    WorldTransform pose;
    if (!TryComputeHierarchyWorldPoseAtDepth(world, entity, pose, 0))
        return false;
    outMatrix = pose.ToMatrix4x4(pose.GetPosition() - WorldPosition{});
    return true;
}

bool TryComputeEntityWorldMatrix(ECS::World* world, ECS::EntityHandle entity, Matrix4x4& outMatrix)
{
    return TryComputeHierarchyWorldMatrix(world, entity, outMatrix);
}

bool TrySetPhysicsBodyWorldPose(ECS::World* world,
                                ECS::EntityHandle entity,
                                const WorldPosition& worldPos,
                                const Quaternion& worldRot)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
    {
        return false;
    }

    auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
    if (!pw)
    {
        return false;
    }

    auto* body = world->GetComponentForWrite<PhysicsBody>(entity);
    if (!body || !body->initialized || !pw->IsBodyValid(body->body))
    {
        return false;
    }

    Physics::Transform pt{};
    pt.position = worldPos;
    const auto& wq = worldRot.GetGLM();
    pt.rotation = Physics::Quaternion(wq.w, wq.x, wq.y, wq.z);
    pw->SetBodyTransform(body->body, pt, Physics::ActivationMode::Activate);
    pw->SetLinearVelocity(body->body, Physics::Vector3(0.0f, 0.0f, 0.0f));
    pw->SetAngularVelocity(body->body, Physics::Vector3(0.0f, 0.0f, 0.0f));

    body->prevPhysicsTransform = pt;
    body->hasPrevPhysicsTransform = true;
    return true;
}

} // namespace GameEngine::Editor
