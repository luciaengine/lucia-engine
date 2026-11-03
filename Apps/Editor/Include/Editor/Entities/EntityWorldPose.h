#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

/// Decomposed translation / rotation / scale of an entity, in local or world space.
struct EntityTRS
{
    Mathematics::Vector3 Position;
    Mathematics::Quaternion Rotation;
    Mathematics::Vector3 Scale{1.0f, 1.0f, 1.0f};
};

/// Composes a child's local TRS under its parent's world TRS, matching
/// Transform::FromTRS and TransformHierarchySystem:
/// worldPos = parentPos + R(parentRot) * (parentScale * localPos).
EntityTRS CombineTRS(const EntityTRS& parent, const EntityTRS& local);

/// World TRS rebuilt from the local Transform and the Parent chain.
///
/// Reads Transform, never the cached WorldTransform: the editor writes local
/// transforms in response to pointer and field edits, and TransformHierarchySystem
/// refreshes WorldTransform later in the frame, so the cache lags one edit behind.
/// An invalid world or entity yields the identity; a missing or dead parent
/// makes the world TRS equal the local one.
EntityTRS ComputeWorldTRSForEntity(ECS::World* world, ECS::EntityHandle handle);

/// World TRS of the entity's parent, or the identity for a root entity.
void GetParentWorldTRS(ECS::World* world,
                       ECS::EntityHandle handle,
                       Mathematics::Vector3& outPos,
                       Mathematics::Quaternion& outRot,
                       Mathematics::Vector3& outScale);

/// Inverse of CombineTRS for position: the local position that places the child
/// at worldPos under the given parent world TRS. A zero parent scale axis is left
/// undivided.
Mathematics::Vector3 ComputeLocalPositionFromWorld(const Mathematics::Vector3& worldPos,
                                                   const Mathematics::Vector3& parentPos,
                                                   const Mathematics::Quaternion& parentRot,
                                                   const Mathematics::Vector3& parentScale);

/// Sector-local world matrix rebuilt from the local Transforms up the Parent chain.
///
/// Like ComputeWorldTRSForEntity it reads Transform rather than the cached
/// WorldTransform, so it sees an edit made earlier in the same frame. An entity
/// without a Transform contributes its cached WorldTransform. Returns false for an
/// invalid entity, an entity with neither component, or a parent chain deeper than
/// the hierarchy depth limit.
bool TryComputeHierarchyWorldMatrix(ECS::World* world,
                                    ECS::EntityHandle entity,
                                    Mathematics::Matrix4x4& outMatrix);

/// Absolute world matrix: the hierarchy world matrix composed with the entity's
/// WorldSectorCoord through Components::ComposeEffectiveWorldTransform, the same
/// composition the render and pick paths use. Equal to the hierarchy matrix for an
/// entity without a sector tag.
bool TryComputeEntityWorldMatrix(ECS::World* world,
                                 ECS::EntityHandle entity,
                                 Mathematics::Matrix4x4& outMatrix);

/// Moves the entity's live physics body to an editor-authored world pose.
///
/// An editor pose is a placement, not a motion: the body's linear and angular
/// velocity are zeroed so it rests where it was put instead of carrying the
/// velocity it had before the edit. The previous-pose snapshot is set to the
/// new pose so render interpolation does not smear across the teleport.
/// Returns false when the entity has no initialized body in a live physics world.
bool TrySetPhysicsBodyWorldPose(ECS::World* world,
                                ECS::EntityHandle entity,
                                const Mathematics::Vector3& worldPos,
                                const Mathematics::Quaternion& worldRot);

} // namespace GameEngine::Editor
