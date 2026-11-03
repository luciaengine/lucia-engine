#include "Editor/Entities/EntityWorldPose.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include <cstring>

namespace GameEngine::Editor
{

using Components::Parent;
using Components::PhysicsBody;
using Components::Transform;
using Components::WorldTransform;
using Mathematics::Matrix4x4;
using Mathematics::Quaternion;
using Mathematics::Vector3;

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

Matrix4x4 MatrixFromRaw(const float matrix[16])
{
    Matrix4x4 out = Matrix4x4::Identity();
    std::memcpy(out.Data(), matrix, sizeof(float) * 16u);
    return out;
}

bool TryComputeHierarchyWorldMatrixAtDepth(ECS::World* world,
                                           ECS::EntityHandle entity,
                                           Matrix4x4& outMatrix,
                                           int depth)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity) || depth > kMaxHierarchyDepth)
    {
        return false;
    }

    if (const auto* transform = world->GetComponent<Transform>(entity))
    {
        const Matrix4x4 local = MatrixFromRaw(transform->matrix);
        if (const auto* parent = world->GetComponent<Parent>(entity); parent && parent->parent.IsValid())
        {
            Matrix4x4 parentWorld;
            if (TryComputeHierarchyWorldMatrixAtDepth(world, parent->parent, parentWorld, depth + 1))
            {
                outMatrix = parentWorld * local;
                return true;
            }
        }

        outMatrix = local;
        return true;
    }

    if (const auto* worldTransform = world->GetComponent<WorldTransform>(entity))
    {
        outMatrix = MatrixFromRaw(worldTransform->matrix);
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

    const Vector3 scaledLocal(
        local.Position.x * parent.Scale.x,
        local.Position.y * parent.Scale.y,
        local.Position.z * parent.Scale.z);
    out.Position = parent.Position + parent.Rotation.Rotate(scaledLocal);

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
        ECS::Entity parentEntity(world, parentComp->parent);
        if (parentEntity.IsValid())
        {
            return CombineTRS(ComputeWorldTRSForEntity(world, parentComp->parent), local);
        }
    }

    return local;
}

void GetParentWorldTRS(ECS::World* world,
                       ECS::EntityHandle handle,
                       Vector3& outPos,
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

    const EntityTRS parentWorld = ComputeWorldTRSForEntity(world, parentComp->parent);
    outPos = parentWorld.Position;
    outRot = parentWorld.Rotation;
    outScale = parentWorld.Scale;
}

Vector3 ComputeLocalPositionFromWorld(const Vector3& worldPos,
                                      const Vector3& parentPos,
                                      const Quaternion& parentRot,
                                      const Vector3& parentScale)
{
    // parentScale * localPos = R(parentRot^-1) * (worldPos - parentPos)
    const Vector3 unrotated = parentRot.Conjugated().Rotate(worldPos - parentPos);

    return Vector3(
        (parentScale.x != 0.0f) ? (unrotated.x / parentScale.x) : unrotated.x,
        (parentScale.y != 0.0f) ? (unrotated.y / parentScale.y) : unrotated.y,
        (parentScale.z != 0.0f) ? (unrotated.z / parentScale.z) : unrotated.z);
}

bool TryComputeHierarchyWorldMatrix(ECS::World* world, ECS::EntityHandle entity, Matrix4x4& outMatrix)
{
    return TryComputeHierarchyWorldMatrixAtDepth(world, entity, outMatrix, 0);
}

bool TryComputeEntityWorldMatrix(ECS::World* world, ECS::EntityHandle entity, Matrix4x4& outMatrix)
{
    Matrix4x4 hierarchyWorld;
    if (!TryComputeHierarchyWorldMatrix(world, entity, hierarchyWorld))
    {
        return false;
    }

    WorldTransform sectorLocal{};
    std::memcpy(sectorLocal.matrix, hierarchyWorld.Data(), sizeof(sectorLocal.matrix));
    const auto* sector = world->GetComponent<Components::WorldSectorCoord>(entity);
    const WorldTransform composed =
        Components::ComposeEffectiveWorldTransform(sectorLocal, sector, Components::kWorldSectorSize);
    outMatrix = MatrixFromRaw(composed.matrix);
    return true;
}

bool TrySetPhysicsBodyWorldPose(ECS::World* world,
                                ECS::EntityHandle entity,
                                const Vector3& worldPos,
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
    pt.position = Physics::Vector3(worldPos.x, worldPos.y, worldPos.z);
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
