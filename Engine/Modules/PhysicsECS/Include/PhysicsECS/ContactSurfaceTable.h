#pragma once

#include "AssetCore/GUID.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsMaterial.h"
#include "Types/Types.h"

#include <filesystem>

namespace GameEngine
{
class AssetEventDispatcher;

namespace Physics
{
class PhysicsWorld;
} // namespace Physics

namespace PhysicsECS
{

/**
 * @brief Bits the engine's default contact predicate reads out of a body's
 * `BodySettings::contactFlags`.
 *
 * Physics assigns these no meaning; the predicate below owns it. A game that
 * installs its own callback is free to redefine every bit.
 */
constexpr uint32 kContactFlagOneWay = 1u << 0;      // solid only to a top-face landing
constexpr uint32 kContactFlagIgnoreOneWay = 1u << 1; // passes through one-way surfaces

/**
 * @brief The engine's index space for contact materials, and the default
 * contact-modify predicate that consumes it.
 *
 * `Physics` resolves materials by opaque index and never sees a GUID or a file;
 * this is the layer that closes that gap, because PhysicsECS is where assets and
 * physics are allowed to meet. Index 0 is reserved for "no authored material" —
 * a contact where neither side carries one keeps the friction and restitution
 * the backend combined from the bodies, so installing the predicate does not
 * silently retune every existing collider in the engine.
 *
 * THREADING — the same contract the backend's per-body side table holds to.
 * PhysicsInit grows the table before PhysicsStep. Additional systems that
 * resolve materials must declare dependencies separating their writes from
 * each other and from PhysicsStep. Physics workers read the table during Step;
 * DrainPendingReloads runs at the head of PhysicsStep, before those reads. A
 * reload overwrites its slot in place. Only ResolveMaterial and AddMaterial
 * append slots, outside PhysicsStep (in PhysicsInit or an ordered system).
 * Indices already baked into bodies stay valid.
 */
class ContactSurfaceTable
{
public:
    /** Index meaning "unauthored" — the contact keeps the backend's own values. */
    static constexpr uint32 kNoMaterial = 0;

    /**
     * Index for the .physmat `guid`, loading it on first use and caching the
     * result. A null GUID, one the asset registry cannot resolve, and a file
     * that fails to decode all return kNoMaterial. Grows the table — see the
     * threading contract above.
     */
    static uint32 ResolveMaterial(const GUID& guid);

    /**
     * The registry-free form ResolveMaterial delegates to, for a .physmat whose
     * location is already known. Grows the table — see the threading contract
     * above.
     */
    static uint32 ResolveMaterialFromFile(const GUID& guid, const std::filesystem::path& path);

    /** Index for a material with no asset behind it. Grows the table. */
    static uint32 AddMaterial(const Physics::PhysicsMaterial& material);

    /**
     * Subscribes to `dispatcher` so an edited .physmat re-reads into the slot it
     * already occupies. Replaces any previous subscription.
     *
     * The notification arrives on the file-watcher thread and is only queued;
     * DrainPendingReloads() does the re-read from the PhysicsStep wave.
     */
    static void SubscribeToAssetChanges(AssetEventDispatcher& dispatcher);

    /** Drops the subscription installed by SubscribeToAssetChanges. */
    static void UnsubscribeFromAssetChanges();

    /**
     * Re-reads every .physmat queued since the last call, overwriting its
     * material in place. Called at the head of PhysicsStep, before the step it
     * precedes — never while one is running, because the physics workers read
     * these slots.
     */
    static void DrainPendingReloads();

    /** The material at `index`; the default-constructed one for kNoMaterial or out of range. */
    static const Physics::PhysicsMaterial& MaterialAt(uint32 index);

    /**
     * Installs Decide() as `world`'s contact-modify callback. Called for every
     * world the engine creates, so one-way tiles and .physmat surfaces are
     * engine behaviour rather than something each game re-implements.
     */
    static void Install(Physics::PhysicsWorld& world);

    /**
     * Drops every resolved material and every queued reload; the index space
     * restarts at kNoMaterial. Leaves the asset-change subscription alone.
     */
    static void Reset();

    /**
     * The default predicate. Exposed so a game that installs its own callback
     * can still delegate to the engine behaviour instead of reimplementing it.
     *
     * One-way: a body flagged kContactFlagOneWay is solid only where its top
     * face meets something that is not moving away from it, so a character
     * jumps up through the platform and lands on it coming down. "Top" is world
     * +Y — the predicate has no per-body frame to read, and tile layers are
     * authored in the XY plane. A body flagged kContactFlagIgnoreOneWay passes
     * through regardless; that is the drop-through switch, and flipping it must
     * also wake the bodies involved (see PhysicsWorld::SetBodyContactFlags).
     *
     * Surface: friction and restitution come from combining the two materials,
     * but only when at least one side actually carries one.
     */
    static Physics::ContactModification Decide(void* context, const Physics::ContactBody& a,
                                               const Physics::ContactBody& b,
                                               const Physics::Vector3& normal,
                                               const Physics::Vector3& relativeVelocity);
};

} // namespace PhysicsECS
} // namespace GameEngine
