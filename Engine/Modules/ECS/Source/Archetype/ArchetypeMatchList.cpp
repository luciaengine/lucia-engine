#include "ECS/Archetype/ArchetypeMatchList.h"

#include "ECS/World.h"
#include "ECS/Components.h"
#include "Memory/Arena.h"
#include "Types/GeometricReserve.h"

#include <algorithm>
#include <limits>

namespace GameEngine::ECS
{

namespace
{
// The copy a list keeps of one filter member: empty where the member is null, which the
// list's own filter then leaves null too.
ComponentSignature CopyOrEmpty(const ComponentSignature* signature)
{
    return signature != nullptr ? *signature : ComponentSignature{};
}

bool SameMember(const ComponentSignature* own, const ComponentSignature* other)
{
    if (own == nullptr || other == nullptr)
        return own == other;
    return *own == *other;
}
} // namespace

ArchetypeMatchList::ArchetypeMatchList(const ArchetypeFilter& filter)
    : m_Required(CopyOrEmpty(filter.Required))
    , m_Include(CopyOrEmpty(filter.Include))
    , m_Exclude(CopyOrEmpty(filter.Exclude))
    , m_DisabledExclude(CopyOrEmpty(filter.DisabledExclude))
    , m_DisabledInclude(CopyOrEmpty(filter.DisabledInclude))
{
    m_Filter.Required = filter.Required != nullptr ? &m_Required : nullptr;
    m_Filter.Include = filter.Include != nullptr ? &m_Include : nullptr;
    m_Filter.Exclude = filter.Exclude != nullptr ? &m_Exclude : nullptr;
    m_Filter.DisabledExclude = filter.DisabledExclude != nullptr ? &m_DisabledExclude : nullptr;
    m_Filter.DisabledInclude = filter.DisabledInclude != nullptr ? &m_DisabledInclude : nullptr;
}

bool ArchetypeMatchList::HasFilter(const ArchetypeFilter& filter) const
{
    return SameMember(m_Filter.Required, filter.Required) && SameMember(m_Filter.Include, filter.Include) &&
           SameMember(m_Filter.Exclude, filter.Exclude) &&
           SameMember(m_Filter.DisabledExclude, filter.DisabledExclude) &&
           SameMember(m_Filter.DisabledInclude, filter.DisabledInclude);
}

bool ArchetypeMatchList::Matches(const Archetype* archetype) const
{
    // Pending schemas remain hidden until required components have been installed.
    const ComponentSignature& signature = archetype->GetSignature();
    return !signature.Contains(GetComponentTypeId<PendingActivation>()) && m_Filter.Matches(signature);
}

void ArchetypeMatchList::AppendMatching(std::span<Archetype* const> archetypes, Memory::Arena& arena)
{
    const auto added = static_cast<std::size_t>(
        std::count_if(archetypes.begin(), archetypes.end(),
                      [this](const Archetype* archetype) { return Matches(archetype); }));
    if (added == 0)
        return;

    std::uint32_t count = m_Count.load(std::memory_order_relaxed);
    Archetype** buffer = m_Archetypes.load(std::memory_order_relaxed);
    if (count + added > m_Capacity)
    {
        // Registration sizes the buffer for every archetype that already matches; later
        // appends grow it geometrically (Types/GeometricReserve.h).
        const std::size_t capacity =
            std::max(count + added, NextGeometricCapacity(m_Capacity, std::numeric_limits<std::uint32_t>::max()));
        Archetype** grown = arena.AllocateArrayForOverwrite<Archetype*>(capacity).data();
        // One store per entry, not std::copy_n: a query reads these entries with no lock once
        // the release below publishes the buffer. ThreadSanitizer checks such stores against
        // those reads, but reports no race against the writes of a memmove.
        for (std::uint32_t entry = 0; entry < count; ++entry)
            grown[entry] = buffer[entry];
        buffer = grown;
        m_Capacity = static_cast<std::uint32_t>(capacity);
        m_Archetypes.store(grown, std::memory_order_release);
    }
    for (Archetype* archetype : archetypes)
    {
        if (Matches(archetype))
            buffer[count++] = archetype;
    }
    m_Count.store(count, std::memory_order_release);
}

void ArchetypeMatchList::Remove(const FlatSet<Archetype*>& removed)
{
    Archetype** archetypes = m_Archetypes.load(std::memory_order_relaxed);
    const std::uint32_t count = m_Count.load(std::memory_order_relaxed);
    Archetype** kept = std::remove_if(archetypes, archetypes + count,
                                      [&removed](Archetype* archetype) { return removed.Contains(archetype); });
    m_Count.store(static_cast<std::uint32_t>(kept - archetypes), std::memory_order_release);
}

void ArchetypeMatchList::Clear()
{
    m_Count.store(0, std::memory_order_release);
}

} // namespace GameEngine::ECS
