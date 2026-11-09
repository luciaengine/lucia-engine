#include "ECS/Archetype/ArchetypeMatchCache.h"

#include "ECS/World.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <initializer_list>

namespace GameEngine::ECS
{

namespace
{
// The cache key of a filter: its five signature hashes in member order, a null member as 0
// (a present but empty signature hashes to 1, so the two never collide). Reads only hashes
// computed already; see ArchetypeMatchCache::Find.
std::uint64_t KeyOf(const ArchetypeFilter& filter)
{
    std::uint64_t key = 0;
    for (const ComponentSignature* signature :
         {filter.Required, filter.Include, filter.Exclude, filter.DisabledExclude, filter.DisabledInclude})
        key = Hashing::HashCombine(key, signature != nullptr ? signature->Hash() : 0);
    return key;
}
} // namespace

ArchetypeMatchCache::ArchetypeMatchCache() : m_Arena("ECS.ArchetypeMatches")
{
}

const ArchetypeMatchList* ArchetypeMatchCache::Find(const ArchetypeFilter& filter) const
{
    ArchetypeMatchList* const* head = m_ListsByKey.Find(KeyOf(filter));
    if (head == nullptr)
        return nullptr;
    for (const ArchetypeMatchList* list = *head; list != nullptr; list = list->m_NextWithSameKey)
    {
        if (list->HasFilter(filter))
            return list;
    }
    return nullptr;
}

const ArchetypeMatchList& ArchetypeMatchCache::Add(const ArchetypeFilter& filter)
{
    ArchetypeMatchList* list = m_Arena.New<ArchetypeMatchList>(filter);
    list->AppendMatching(m_Archetypes, m_Arena);

    ArchetypeMatchList*& head = m_ListsByKey.GetOrInsert(KeyOf(filter));
    list->m_NextWithSameKey = head;
    head = list;
    return *list;
}

void ArchetypeMatchCache::AddArchetype(Archetype* archetype)
{
    m_Archetypes.push_back(archetype);
    for (auto& entry : m_ListsByKey)
    {
        for (ArchetypeMatchList* list = entry.second; list != nullptr; list = list->m_NextWithSameKey)
            list->AppendMatching(std::span<Archetype* const>(&archetype, 1), m_Arena);
    }
}

void ArchetypeMatchCache::RemoveArchetypes(const FlatSet<Archetype*>& removed)
{
    std::erase_if(m_Archetypes, [&removed](Archetype* archetype) { return removed.Contains(archetype); });
    for (auto& entry : m_ListsByKey)
    {
        for (ArchetypeMatchList* list = entry.second; list != nullptr; list = list->m_NextWithSameKey)
            list->Remove(removed);
    }
}

void ArchetypeMatchCache::Clear()
{
    m_Archetypes.clear();
    for (auto& entry : m_ListsByKey)
    {
        for (ArchetypeMatchList* list = entry.second; list != nullptr; list = list->m_NextWithSameKey)
            list->Clear();
    }
}

} // namespace GameEngine::ECS
