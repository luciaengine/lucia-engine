#pragma once

#include "ECS/Archetype/ArchetypeMatchList.h"
#include "ECS/ECS.h"
#include "Memory/Arena.h"
#include "Types/FlatMap.h"
#include "Types/FlatSet.h"

#include <cstdint>
#include <vector>

namespace GameEngine::ECS
{

class Archetype;

/// A World's archetype matches: one ArchetypeMatchList per distinct query filter, kept current as archetypes are
/// created and destroyed. Filters are keyed by content and each list owns copies of its signatures, so a module
/// unload leaves no dangling key. Find needs the World's lock, shared or exclusive; every other member, exclusive.
class ArchetypeMatchCache
{
  public:
    ArchetypeMatchCache();

    ArchetypeMatchCache(const ArchetypeMatchCache&) = delete;
    ArchetypeMatchCache& operator=(const ArchetypeMatchCache&) = delete;
    ArchetypeMatchCache(ArchetypeMatchCache&&) = delete;
    ArchetypeMatchCache& operator=(ArchetypeMatchCache&&) = delete;

    /// The list for `filter`, or null when no query with this filter has run on the World.
    /// Every signature of `filter` has its hash computed already when it is shared between
    /// threads (SignatureOf and the Query statics compute it as they are built).
    const ArchetypeMatchList* Find(const ArchetypeFilter& filter) const;

    /// Registers `filter` and matches it against every live archetype. Allocates; a filter is
    /// registered once per World.
    const ArchetypeMatchList& Add(const ArchetypeFilter& filter);

    /// A new archetype, appended to every list whose filter it matches.
    void AddArchetype(Archetype* archetype);

    /// Archetypes the World destroyed, dropped from every list.
    void RemoveArchetypes(const FlatSet<Archetype*>& removed);

    /// The World dropped every archetype. The registered filters stay, with empty lists.
    void Clear();

  private:
    /// Every list and every list buffer: their addresses stay fixed until the World is destroyed.
    Memory::Arena m_Arena;
    FlatMap<std::uint64_t, ArchetypeMatchList*> m_ListsByKey;
    /// Every live archetype, in creation order: what a newly registered filter is matched against.
    std::vector<Archetype*> m_Archetypes;
};

} // namespace GameEngine::ECS
