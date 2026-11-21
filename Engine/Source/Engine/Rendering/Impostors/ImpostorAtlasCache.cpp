#include "Engine/Rendering/Impostors/ImpostorAtlasCache.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace GameEngine::Engine::Renderer
{
namespace
{

constexpr uint32 kMinimumMipTileSize = 4;
constexpr uint64 kColorBytesPerPixel = 4;        // RGBA8
constexpr uint64 kNormalDepthBytesPerPixel = 8;  // RGBA16F

} // namespace

uint32 ImpostorAtlasMipCount(const ImpostorAtlasKey& key)
{
    if (!key.FilterMips || key.FrameSize < kMinimumMipTileSize || !std::has_single_bit(key.FrameSize))
        return 1;
    return std::bit_width(key.FrameSize / kMinimumMipTileSize);
}

uint64 ImpostorAtlasBytes(const ImpostorAtlasKey& key)
{
    const uint64 frames = static_cast<uint64>(key.FramesPerSide) * key.FramesPerSide;
    uint64 pixels = 0;
    const uint32 mipCount = ImpostorAtlasMipCount(key);
    for (uint32 mip = 0; mip < mipCount; ++mip)
    {
        const uint64 side = key.FrameSize >> mip;
        pixels += side * side;
    }
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
    m_Entries.InsertOrAssign(key, Entry{State::Pending, frame});
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
    // An atlas used in this frame stays: its instances already draw it.
    uint64 freed = 0;
    while (freed < neededBytes)
    {
        const ImpostorAtlasKey* oldest = nullptr;
        uint64 oldestFrame = std::numeric_limits<uint64>::max();
        for (const auto& [key, entry] : m_Entries)
        {
            if (entry.LastUsedFrame < frame && entry.LastUsedFrame < oldestFrame)
            {
                oldest = &key;
                oldestFrame = entry.LastUsedFrame;
            }
        }
        if (!oldest)
            return;
        const ImpostorAtlasKey victim = *oldest;
        const uint64 bytes = ImpostorAtlasBytes(victim);
        freed += bytes;
        m_UsedBytes -= bytes;
        outEvicted.push_back(victim);
        m_Entries.Erase(victim);
    }
}

} // namespace GameEngine::Engine::Renderer
