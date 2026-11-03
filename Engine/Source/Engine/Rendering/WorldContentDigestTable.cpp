#include "Engine/Rendering/WorldContentDigestTable.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <vector>

namespace GameEngine::Engine::Renderer
{

struct WorldContentDigestTable::State
{
    struct Entry
    {
        uint64 WorldId = 0;
        uint64 Digest = 0;
        uint32 TrackCount = 0;
    };

    // A handful of live worlds at most: a flat scan beats a node-based map.
    Entry* Find(uint64 worldId)
    {
        const auto it = std::find_if(Entries.begin(), Entries.end(),
                                     [worldId](const Entry& entry) { return entry.WorldId == worldId; });
        return it == Entries.end() ? nullptr : &*it;
    }

    void Release(uint64 worldId)
    {
        std::lock_guard lock(Mutex);
        Entry* entry = Find(worldId);
        assert(entry && "released a world content digest that was never tracked");
        if (!entry || --entry->TrackCount > 0)
            return;
        *entry = Entries.back();
        Entries.pop_back();
    }

    std::mutex Mutex;
    std::vector<Entry> Entries;
};

WorldContentDigestTable::WorldContentDigestTable() : m_State(std::make_shared<State>()) {}

WorldContentDigestTable::~WorldContentDigestTable() = default;

ScopedSubscription WorldContentDigestTable::Track(uint64 worldId)
{
    {
        std::lock_guard lock(m_State->Mutex);
        if (State::Entry* entry = m_State->Find(worldId))
            ++entry->TrackCount;
        else
            m_State->Entries.push_back({worldId, 0, 1});
    }
    State* state = m_State.get();
    return ScopedSubscription(m_State, [state, worldId] { state->Release(worldId); });
}

void WorldContentDigestTable::Set(uint64 worldId, uint64 digest)
{
    std::lock_guard lock(m_State->Mutex);
    State::Entry* entry = m_State->Find(worldId);
    assert(entry && "Track the world before setting its content digest");
    if (entry)
        entry->Digest = digest;
}

uint64 WorldContentDigestTable::Get(uint64 worldId) const
{
    std::lock_guard lock(m_State->Mutex);
    const State::Entry* entry = m_State->Find(worldId);
    return entry ? entry->Digest : 0;
}

} // namespace GameEngine::Engine::Renderer
