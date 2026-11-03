#pragma once

#include "Physics/PhysicsQuery.h"

namespace GameEngine::Physics
{
enum class ContactState : uint8
{
    Begin,
    Persist,
    End,
};

struct ContactEvent
{
    BodyHandle bodyA{};
    BodyHandle bodyB{};
    uint64 userDataA = 0;
    uint64 userDataB = 0;
    ContactState state = ContactState::Persist;

    Vector3 point{0.0f, 0.0f, 0.0f};
    Vector3 normal{0.0f, 1.0f, 0.0f};
    float32 impulse = 0.0f;
};

/** One side of a contact, as snapshotted at body creation. */
struct ContactBody
{
    BodyHandle body{};
    uint64 userData = 0;
    uint32 contactFlags = 0;
    uint32 materialIndex = 0;
    float32 friction = 0.5f;
    float32 restitution = 0.1f;
};

/** What the contact-modify callback decides about one contact. */
struct ContactModification
{
    bool accept = true;
    float32 friction = 0.5f;
    float32 restitution = 0.1f;

    // When false, `friction` and `restitution` are ignored and the contact
    // keeps whatever the backend combined from the two bodies. A predicate that
    // only decides acceptance — or that has no material for either side — must
    // say so rather than flattening every contact in the world onto its
    // defaults.
    bool overrideSurface = true;
};

/**
 * Decides, per contact, whether it is processed at all and with what surface
 * response — the seam behind one-way platforms and material combine modes.
 *
 * CONTRACT — this runs on physics worker threads, with all bodies locked:
 *  - It must be pure: no allocation, no locking, no physics mutation, no ECS
 *    access. Every input it needs is in its arguments or behind `context`,
 *    which must stay immutable for the duration of a step.
 *  - It is invoked more than once per contact per step (see the backend), so
 *    it must be deterministic: same inputs, same decision.
 *
 * `normal` points from body A's surface toward B; `relativeVelocity` is B's
 * velocity at the contact minus A's. Landing on a surface therefore has a
 * negative dot(normal, relativeVelocity).
 */
using ContactModifyCallback = ContactModification (*)(void* context, const ContactBody& a,
                                                      const ContactBody& b, const Vector3& normal,
                                                      const Vector3& relativeVelocity);

struct TriggerEvent
{
    BodyHandle trigger{};
    BodyHandle other{};
    uint64 triggerUserData = 0;
    uint64 otherUserData = 0;
    bool isEntering = false;
};

} // namespace GameEngine::Physics

