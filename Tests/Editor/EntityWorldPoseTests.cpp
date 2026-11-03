#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include "Editor/Entities/EntityWorldPose.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"

using GameEngine::Components::Parent;
using GameEngine::Components::PhysicsBody;
using GameEngine::Components::PhysicsCollider;
using GameEngine::Components::SphereColliderShape;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldSectorCoord;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::Entity;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Editor::CombineTRS;
using GameEngine::Editor::ComputeLocalPositionFromWorld;
using GameEngine::Editor::ComputeWorldTRSForEntity;
using GameEngine::Editor::EntityTRS;
using GameEngine::Editor::GetParentWorldTRS;
using GameEngine::Editor::TryComputeEntityWorldMatrix;
using GameEngine::Editor::TryComputeHierarchyWorldMatrix;
using GameEngine::Editor::TrySetPhysicsBodyWorldPose;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;
using GameEngine::PhysicsECS::PhysicsWorldService;

namespace
{
constexpr float kPoseTolerance = 1e-4f;

void ExpectVectorNear(const Vector3& actual, const Vector3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, kPoseTolerance);
    EXPECT_NEAR(actual.y, expected.y, kPoseTolerance);
    EXPECT_NEAR(actual.z, expected.z, kPoseTolerance);
}

EntityHandle CreateTransformEntity(World& world, const Vector3& position, const Quaternion& rotation, const Vector3& scale)
{
    Entity entity = world.Create();
    entity.Set(Transform::FromTRS(position, rotation, scale));
    return entity.GetHandle();
}

// A child's world position survives the round trip through its parent's world TRS:
// ComputeLocalPositionFromWorld is the inverse of CombineTRS for position.
TEST(EntityWorldPoseTests, LocalPositionFromWorldInvertsCombineUnderRotatedScaledParent)
{
    World world;
    const Quaternion parentRot = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.7f);
    const EntityHandle parent =
        CreateTransformEntity(world, Vector3(3.0f, -1.0f, 2.0f), parentRot, Vector3(2.0f, 0.5f, 4.0f));
    const EntityHandle child =
        CreateTransformEntity(world, Vector3(1.0f, 2.0f, -3.0f), Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
    world.ProcessCommands();
    world.AddComponentImmediate(child, Parent{parent});

    const EntityTRS childWorld = ComputeWorldTRSForEntity(&world, child);

    Vector3 parentPos;
    Quaternion parentRotOut;
    Vector3 parentScale;
    GetParentWorldTRS(&world, child, parentPos, parentRotOut, parentScale);

    const EntityTRS recombined = CombineTRS(EntityTRS{parentPos, parentRotOut, parentScale},
                                            EntityTRS{Vector3(1.0f, 2.0f, -3.0f), Quaternion::Identity(),
                                                      Vector3(1.0f, 1.0f, 1.0f)});
    ExpectVectorNear(recombined.Position, childWorld.Position);

    const Vector3 local = ComputeLocalPositionFromWorld(childWorld.Position, parentPos, parentRotOut, parentScale);
    ExpectVectorNear(local, Vector3(1.0f, 2.0f, -3.0f));
}

Vector3 TranslationOf(const Matrix4x4& m)
{
    return Vector3(m[3].x, m[3].y, m[3].z);
}

// The matrix walk and the TRS walk read the same parent chain and must agree on
// where a child sits.
TEST(EntityWorldPoseTests, HierarchyWorldMatrixMatchesWorldTRS)
{
    World world;
    const Quaternion parentRot = Quaternion::FromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), 0.4f);
    const EntityHandle parent =
        CreateTransformEntity(world, Vector3(-2.0f, 4.0f, 1.0f), parentRot, Vector3(1.5f, 1.5f, 1.5f));
    const EntityHandle child =
        CreateTransformEntity(world, Vector3(0.5f, 0.0f, 2.0f), Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
    world.ProcessCommands();
    world.AddComponentImmediate(child, Parent{parent});

    Matrix4x4 matrix;
    ASSERT_TRUE(TryComputeHierarchyWorldMatrix(&world, child, matrix));
    ExpectVectorNear(TranslationOf(matrix), ComputeWorldTRSForEntity(&world, child).Position);
}

// A sector-tagged entity renders and picks at sector * kWorldSectorSize + its
// sector-local position; the editor's absolute world matrix lands on the same point.
TEST(EntityWorldPoseTests, EntityWorldMatrixComposesOwnSector)
{
    World world;
    const EntityHandle e =
        CreateTransformEntity(world, Vector3(1.0f, 2.0f, 3.0f), Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
    world.ProcessCommands();
    world.AddComponentImmediate(e, WorldSectorCoord{2, 0, -1});

    Matrix4x4 hierarchy;
    ASSERT_TRUE(TryComputeHierarchyWorldMatrix(&world, e, hierarchy));
    ExpectVectorNear(TranslationOf(hierarchy), Vector3(1.0f, 2.0f, 3.0f));

    Matrix4x4 composed;
    ASSERT_TRUE(TryComputeEntityWorldMatrix(&world, e, composed));
    const float sectorSize = GameEngine::Components::kWorldSectorSize;
    ExpectVectorNear(TranslationOf(composed), Vector3(1.0f + 2.0f * sectorSize, 2.0f, 3.0f - sectorSize));
}

class EntityWorldPosePhysicsTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();

        GameEngine::Physics::PhysicsWorldSettings settings{};
        settings.gravity = GameEngine::Physics::Vector3(0.0f, -9.81f, 0.0f);
        settings.numThreads = 1;
        settings.maxBodies = 64;
        settings.maxBodyPairs = 64;
        settings.maxContactConstraints = 64;
        settings.tempAllocatorBytes = 4u * 1024u * 1024u;
        PhysicsWorldService::Initialize(settings);
    }

    void TearDown() override
    {
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
    }

    EntityHandle CreateDynamicBody(const Vector3& position)
    {
        const EntityHandle e =
            CreateTransformEntity(m_World, position, Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
        m_World.ProcessCommands();

        WorldTransform wt{};
        const Transform local = Transform::FromTRS(position, Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
        std::memcpy(wt.matrix, local.matrix, sizeof(wt.matrix));
        m_World.AddComponentImmediate(e, wt);
        m_World.AddComponentImmediate(e, PhysicsCollider{});

        SphereColliderShape sphere{};
        sphere.radius = 0.5f;
        m_World.AddComponentImmediate(e, sphere);

        PhysicsBody body{};
        body.motionType = GameEngine::Physics::MotionType::Dynamic;
        body.mass = 1.0f;
        m_World.AddComponentImmediate(e, body);

        m_Init.Update(m_World, 1.0f / 60.0f);
        return e;
    }

    World m_World;
    GameEngine::PhysicsECS::PhysicsInitSystem m_Init;
};

// An editor pose is a placement: whether it comes from the gizmo or from a typed
// inspector value, the body rests where it was put instead of keeping its velocity.
TEST_F(EntityWorldPosePhysicsTests, SettingPoseTeleportsBodyAndZeroesVelocity)
{
    const EntityHandle e = CreateDynamicBody(Vector3(0.0f, 5.0f, 0.0f));
    auto* pw = PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);
    const auto* body = m_World.GetComponent<PhysicsBody>(e);
    ASSERT_NE(body, nullptr);
    ASSERT_TRUE(body->initialized) << "PhysicsInitSystem never provisioned the body";

    pw->SetLinearVelocity(body->body, GameEngine::Physics::Vector3(4.0f, -2.0f, 1.0f));
    pw->SetAngularVelocity(body->body, GameEngine::Physics::Vector3(0.0f, 3.0f, 0.0f));

    const Vector3 target(10.0f, 1.0f, -6.0f);
    ASSERT_TRUE(TrySetPhysicsBodyWorldPose(&m_World, e, target, Quaternion::Identity()));

    ExpectVectorNear(pw->GetBodyTransform(body->body).position, target);
    ExpectVectorNear(pw->GetLinearVelocity(body->body), Vector3(0.0f, 0.0f, 0.0f));
    ExpectVectorNear(pw->GetAngularVelocity(body->body), Vector3(0.0f, 0.0f, 0.0f));

    const auto* after = m_World.GetComponent<PhysicsBody>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->hasPrevPhysicsTransform);
    ExpectVectorNear(after->prevPhysicsTransform.position, target);
}

TEST_F(EntityWorldPosePhysicsTests, EntityWithoutBodyReportsNoPoseSet)
{
    const EntityHandle e =
        CreateTransformEntity(m_World, Vector3(0.0f, 0.0f, 0.0f), Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
    m_World.ProcessCommands();

    EXPECT_FALSE(TrySetPhysicsBodyWorldPose(&m_World, e, Vector3(1.0f, 2.0f, 3.0f), Quaternion::Identity()));
}
} // namespace
