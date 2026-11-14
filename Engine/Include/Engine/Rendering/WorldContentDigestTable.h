#pragma once

#include "Types/ScopedSubscription.h"
#include "Types/Types.h"

#include <memory>

namespace GameEngine::Engine::Renderer
{

// Per-world content digests a render feature answers IRenderFeature::ContentDigest
// from. The feature is shared by every world, so the per-world ECS system that
// extracts its content writes that world's entry each frame.
//
// An entry lives while a Track subscription for its world lives. The tracking
// system owns the subscription, so the entry goes when the system goes with its
// world; a table that dies first makes that release a no-op. Thread-safe: worlds
// write and read their entries from their own system threads.
class WorldContentDigestTable
{
  public:
    WorldContentDigestTable();
    ~WorldContentDigestTable();

    WorldContentDigestTable(const WorldContentDigestTable&) = delete;
    WorldContentDigestTable& operator=(const WorldContentDigestTable&) = delete;

    // Starts an entry for worldId with digest 0. Tracking a world twice (a
    // system replaced on a plugin reload) keeps one entry until both
    // subscriptions are released.
    [[nodiscard]] ScopedSubscription Track(uint64 worldId);

    // Stores worldId's digest. The world must be tracked.
    void Set(uint64 worldId, uint64 digest);

    // worldId's digest, or 0 when the world is not tracked.
    uint64 Get(uint64 worldId) const;

  private:
    struct State;
    std::shared_ptr<State> m_State;
};

} // namespace GameEngine::Engine::Renderer
