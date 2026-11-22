#include "Engine/Rendering/Impostors/ImpostorAtlasCache.h"

#include <algorithm>
#include <limits>

namespace GameEngine::Engine::Renderer
{
namespace
{

constexpr uint64 kColorBytesPerPixel = 4;        // RGBA8
constexpr uint64 kNormalDepthBytesPerPixel = 8;  // RGBA16F

} // namespace

uint64 ImpostorAtlasBytes(const ImpostorAtlasKey& key)
{
    const uint64 frames = static_cast<uint64>(key.FramesPerSide) * key.FramesPerSide;
    const uint64 pixels = static_cast<uint64>(key.FrameSize) * key.FrameSize;
    return frames * pixels * (kColorBytesPerPixel + kNormalDepthBytesPerPixel);
}

ImpostorAtlasCache::ImpostorAtlasCache(uint64 budgetBytes)
    : m_BudgetBytes(budgetBytes)
{
}

ImpostorAtlasCache::State ImpostorAtlasCache::GetState(const ImpostorAtlasKey& key) const
{
    const Entry* entry = m_Entries.Find(key);
    return entry ? entry->Status : State::Absent;
}

ImpostorAtlasCache::State ImpostorAtlasCache::Request(const ImpostorAtlasKey& key,
                                                      uint64 frame,
                                                      std::vector<ImpostorAtlasKey>& outEvicted)
{
    if (Entry* entry = m_Entries.Find(key))
    {
        entry->LastUsedFrame = frame;
        return entry->Status;
    }
    const uint64 bytes = ImpostorAtlasBytes(key);
    if (bytes > m_BudgetBytes)
        return State::Absent;
    if (m_UsedBytes + bytes > m_BudgetBytes)
        EvictLeastRecentlyUsed(m_UsedBytes + bytes - m_BudgetBytes, frame, outEvicted);
    if (m_UsedBytes + bytes > m_BudgetBytes)
        return State::Absent;
    m_Entries.InsertOrAssign(key, Entry{State::Pending, frame, bytes});
    m_UsedBytes += bytes;
    return State::Pending;
}

void ImpostorAtlasCache::MarkReady(const ImpostorAtlasKey& key)
{
    if (Entry* entry = m_Entries.Find(key))
        entry->Status = State::Ready;
}

void ImpostorAtlasCache::Clear()
{
    m_Entries.Clear();
    m_UsedBytes = 0;
}

void ImpostorAtlasCache::EvictLeastRecentlyUsed(uint64 neededBytes, uint64 frame, std::vector<ImpostorAtlasKey>& outEvicted)
{
    // Atlases used in this frame stay. Check the full eviction capacity before retiring any texture.
    uint64 evictableBytes = 0;
    for (const auto& [key, entry] : m_Entries)
    {
        if (entry.LastUsedFrame < frame)
        {
            evictableBytes += entry.ReservedBytes;
            if (evictableBytes >= neededBytes)
                break;
        }
    }
    if (evictableBytes < neededBytes)
        return;

    uint64 freed = 0;
    while (freed < neededBytes)
    {
        const ImpostorAtlasKey* oldest = nullptr;
        uint64 oldestBytes = 0;
        uint64 oldestFrame = std::numeric_limits<uint64>::max();
        for (const auto& [key, entry] : m_Entries)
        {
            if (entry.LastUsedFrame < frame && entry.LastUsedFrame < oldestFrame)
            {
                oldest = &key;
                oldestBytes = entry.ReservedBytes;
                oldestFrame = entry.LastUsedFrame;
            }
        }
        if (!oldest)
            return;
        const ImpostorAtlasKey victim = *oldest;
        freed += oldestBytes;
        m_UsedBytes -= oldestBytes;
        outEvicted.push_back(victim);
        m_Entries.Erase(victim);
    }
}

} // namespace GameEngine::Engine::Renderer
