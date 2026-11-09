#pragma once

#include "ECS/ECS.h"
#include "Types/FlatSet.h"

#include <atomic>
#include <cstdint>
#include <span>

namespace GameEngine::Memory
{
class Arena;
}

namespace GameEngine::ECS
{

class Archetype;

/// The archetypes one query filter matches in one World; its ArchetypeMatchCache changes it only under the World's
/// exclusive lock. GetArchetypes takes no lock and published entries stay readable, but only a count read under the
/// lock ends at fully built archetypes. Clear and PruneEmptyArchetypes rewrite entries: never while the World iterates.
class ArchetypeMatchList
{
  public:
    /// Copies the filter's signatures: the list outlives the image that built the filter.
    explicit ArchetypeMatchList(const ArchetypeFilter& filter);

    ArchetypeMatchList(const ArchetypeMatchList&) = delete;
    ArchetypeMatchList& operator=(const ArchetypeMatchList&) = delete;
    ArchetypeMatchList(ArchetypeMatchList&&) = delete;
    ArchetypeMatchList& operator=(ArchetypeMatchList&&) = delete;

    /// The matching archetypes as of this call. Archetypes created afterwards are not in the
    /// returned span; it stays valid until the World's next Clear or PruneEmptyArchetypes.
    /// Without the World's lock, its last entries may be archetypes still being built: keep
    /// only the first entries a count taken under the lock names (see the class comment).
    std::span<Archetype* const> GetArchetypes() const
    {
        // Count first: every entry below it is in the buffer published with or before it.
        const std::uint32_t count = m_Count.load(std::memory_order_acquire);
        Archetype* const* archetypes = m_Archetypes.load(std::memory_order_acquire);
        return {archetypes, count};
    }

  private:
    friend class ArchetypeMatchCache;

    /// True if this list was built for a filter with the same five signatures as `filter`.
    bool HasFilter(const ArchetypeFilter& filter) const;
    bool Matches(const Archetype* archetype) const;
    /// Appends, in order, the archetypes of `archetypes` the filter matches. A buffer too small
    /// for them is replaced once by a larger copy taken from `arena`; the old one stays where a
    /// reader may still hold it.
    void AppendMatching(std::span<Archetype* const> archetypes, Memory::Arena& arena);
    void Remove(const FlatSet<Archetype*>& removed);
    void Clear();

    ComponentSignature m_Required;
    ComponentSignature m_Include;
    ComponentSignature m_Exclude;
    ComponentSignature m_DisabledExclude;
    ComponentSignature m_DisabledInclude;
    /// Points at the signatures above, null where the filter it was built from had none.
    ArchetypeFilter m_Filter;

    std::atomic<Archetype**> m_Archetypes{nullptr};
    std::atomic<std::uint32_t> m_Count{0};
    std::uint32_t m_Capacity = 0;

    /// The next list whose filter hashes to the same cache key.
    ArchetypeMatchList* m_NextWithSameKey = nullptr;
};

} // namespace GameEngine::ECS
