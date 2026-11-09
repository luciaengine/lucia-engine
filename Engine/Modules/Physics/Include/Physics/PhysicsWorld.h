#pragma once

#include "Physics/PhysicsConstraints.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsQuery.h"
#include "Physics/PhysicsShapes.h"
#include "Physics/PhysicsTypes.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine::Physics
{
class IPhysicsBackend;

// Backend-agnostic physics world façade. Implementation is delegated to a backend
// (e.g. Jolt) selected at build time.
class PhysicsWorld
{
public:
    using ListenerId = uint64;

    explicit PhysicsWorld(const PhysicsWorldSettings& settings = {});
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    // ---------------------------------------------------------------------
    // Simulation
    // ---------------------------------------------------------------------
    void Step(float32 deltaTime, int32 collisionSteps = 1);
    void OptimizeBroadphase();

    // ---------------------------------------------------------------------
    // Shapes
    // ---------------------------------------------------------------------
    ShapeHandle CreateShape(const ShapeDefinition& definition, const Vector3& bakedScale = Vector3(1, 1, 1));
    void DestroyShape(ShapeHandle shape);
    bool IsShapeValid(ShapeHandle shape) const;

    // In-place heightfield region update; see IPhysicsBackend for contract.
    // Returns false when the backend cannot update in place — caller falls
    // back to the destroy+recreate path.
    bool UpdateHeightFieldRegion(BodyHandle body, ShapeHandle shape,
                                 const float32* allSamples, uint32 gridN,
                                 uint32 x, uint32 z,
                                 uint32 sizeX, uint32 sizeZ);

    // Wake all dynamic bodies whose broadphase AABB intersects the box.
    // Required after any heightfield geometry change so sleeping bodies
    // notice the ground moving.
    void ActivateBodiesInAABB(const AABB& worldBox);

    // ---------------------------------------------------------------------
    // Bodies
    // ---------------------------------------------------------------------
    BodyHandle CreateBody(const BodySettings& settings);
    void DestroyBody(BodyHandle body);
    bool IsBodyValid(BodyHandle body) const;

    Transform GetBodyTransform(BodyHandle body) const;
    void SetBodyTransform(BodyHandle body, const Transform& transform, ActivationMode activation = ActivationMode::Activate);

    // Drive a KINEMATIC body toward `target`, by setting the linear and angular
    // velocity that arrives there in `deltaTime`. Wakes the body.
    //
    // This is the only correct way to move a kinematic platform. SetBodyTransform
    // teleports: the body is relocated outside the solver, so the contact
    // manifolds under whatever rests on it are discarded, the accumulated
    // friction impulses that carry a rider are lost, and the rider is left
    // behind (or tunnels). A velocity-driven body sweeps between poses, keeps
    // its manifolds, and transfers motion through ordinary contact friction —
    // which is what carries dynamic riders for free.
    //
    // `deltaTime` should be the interval over which the simulation will actually
    // advance before the next call. Any residual gap is corrected on the next
    // call, since the velocity is recomputed from the body's real pose each
    // time; error does not accumulate.
    void MoveKinematic(BodyHandle body, const Transform& target, float32 deltaTime);

    Vector3 GetLinearVelocity(BodyHandle body) const;
    void SetLinearVelocity(BodyHandle body, const Vector3& velocity);

    Vector3 GetAngularVelocity(BodyHandle body) const;
    void SetAngularVelocity(BodyHandle body, const Vector3& velocity);

    // World-space center of mass (reference point for buoyancy torque).
    Vector3 GetCenterOfMassPosition(BodyHandle body) const;

    // World-space velocity that `body` imparts to the world-space point
    // `worldPoint`, including the angular contribution — so a point on a
    // rotating platform reports the correct tangential speed, not the
    // platform's linear velocity.
    //
    // This is the engine-side obligation for riders that are not dynamic bodies.
    // A dynamic rider needs nothing: contact friction against a properly moved
    // kinematic body carries it. A character that does its own sweeping instead
    // of being simulated has no contact to draw from, so it queries the surface
    // it is standing on and adds this to its own motion.
    //
    // Zero for a static or invalid body.
    Vector3 GetPointVelocity(BodyHandle body, const Vector3& worldPoint) const;

    // Forces/torques accumulate and apply during the next Step(); impulses
    // change velocity immediately. The "AtPosition" variants apply at a
    // world-space point (producing torque) — used for buoyancy at probe points.
    void AddForce(BodyHandle body, const Vector3& force);
    void AddForceAtPosition(BodyHandle body, const Vector3& force, const Vector3& worldPosition);
    void AddTorque(BodyHandle body, const Vector3& torque);
    void AddImpulse(BodyHandle body, const Vector3& impulse);
    void AddImpulseAtPosition(BodyHandle body, const Vector3& impulse, const Vector3& worldPosition);

    // ---------------------------------------------------------------------
    // Characters
    // ---------------------------------------------------------------------
    CharacterHandle CreateCharacter(const CharacterSettings& settings);
    void DestroyCharacter(CharacterHandle character);
    bool IsCharacterValid(CharacterHandle character) const;

    Transform GetCharacterTransform(CharacterHandle character) const;
    void SetCharacterPosition(CharacterHandle character, const Vector3& position);

    Vector3 GetCharacterLinearVelocity(CharacterHandle character) const;
    void SetCharacterLinearVelocity(CharacterHandle character, const Vector3& velocity);

    bool IsCharacterGrounded(CharacterHandle character) const;
    Vector3 GetCharacterGroundNormal(CharacterHandle character) const;

    // ---------------------------------------------------------------------
    // Queries
    // ---------------------------------------------------------------------
    /// The nearest body along `query.ray` within `query.maxDistance` that `query.filter` sees.
    /// False, with an empty result, when there is none.
    bool RayCast(const RayCastQuery& query, RayCastResult& outResult) const;
    /// Every body along the ray that `query.filter` sees, nearest first (bodies at the same
    /// distance in a fixed order, by body), each once at its nearest hit (a mesh the ray crosses
    /// several times is one result), at most `maxResults` of them.
    /// `maxResults` 0 asks for none: false with an empty list, as BoxOverlap. True when any is hit.
    bool RayCastAll(const RayCastQuery& query, std::vector<RayCastResult>& outResults, uint32 maxResults = 64) const;
    bool BoxOverlap(const BoxOverlapQuery& query, std::vector<OverlapResult>& outResults, uint32 maxResults = 64) const;

    // ---------------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------------
    using ContactCallback = GameEngine::Function<void(const ContactEvent&)>;
    using TriggerCallback = GameEngine::Function<void(const TriggerEvent&)>;

    // Add/remove listeners. Multiple listeners can be registered; they are invoked in registration order.
    // If no listeners exist, the backend will not generate/dispatch events.
    ListenerId AddContactListener(ContactCallback callback);
    ListenerId AddTriggerListener(TriggerCallback callback);
    void RemoveContactListener(ListenerId id);
    void RemoveTriggerListener(ListenerId id);
    void ClearContactListeners();
    void ClearTriggerListeners();

    // Per-contact accept/reject and surface response. Unlike the listeners
    // above — which are queued during the step and dispatched afterwards, on
    // the caller's thread — this runs synchronously on physics worker threads
    // while the step is in flight; see ContactModifyCallback
    // for the contract it must honour. Only one may be registered; passing
    // nullptr clears it and restores plain body-level friction/restitution.
    // `context` must outlive the callback's registration.
    //
    // The callback is only consulted for bodies that are awake. If a decision
    // changes because the context changed — a character starting to drop
    // through a platform it rests on — the caller must wake the affected body
    // (ActivateBodiesInAABB); nothing else can observe that change.
    void SetContactModifyCallback(ContactModifyCallback callback, void* context);

    // Replaces the contact flags snapshotted at body creation. It writes,
    // unsynchronized, the table the callback reads from workers, so the caller
    // must serialize it against the step and against body creation — from an
    // ECS system, that means a schedule wave of its own ahead of PhysicsStep.
    // A body already asleep does not re-evaluate its contacts, so a
    // change that must take effect now (a character asking to drop through the
    // platform it rests on) has to wake the affected bodies as well.
    void SetBodyContactFlags(BodyHandle body, uint32 contactFlags);

    // Convenience (back-compat): replaces all listeners with a single callback.
    void SetContactCallback(ContactCallback callback);
    void SetTriggerCallback(TriggerCallback callback);
    void ProcessEvents();

    // Diagnostic counters for the Monitors panel.
    uint32 GetBodyCount() const { return m_BodyCount; }
    uint32 GetContactPairCount() const { return m_LastContactPairCount; }

private:
    void UpdateBackendEventCallbacks();

    std::unique_ptr<IPhysicsBackend> m_Backend;
    std::vector<std::pair<ListenerId, ContactCallback>> m_ContactListeners;
    std::vector<std::pair<ListenerId, TriggerCallback>> m_TriggerListeners;
    ListenerId m_NextListenerId = 1;

    uint32 m_BodyCount = 0;
    uint32 m_LastContactPairCount = 0;
    uint32 m_AccumContactPairs = 0;
};

} // namespace GameEngine::Physics

