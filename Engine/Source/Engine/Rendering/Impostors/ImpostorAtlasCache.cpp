#include "Engine/Rendering/Impostors/ImpostorAtlasCache.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <limits>

namespace GameEngine::Engine::Renderer
{
namespace
{

constexpr uint32 kMinimumMipTileSize = 4;

constexpr uint64 kColorBytesPerPixel = 4;        // RGBA8
constexpr uint64 kNormalDepthBytesPerPixel = 8;  // RGBA16F
constexpr uint64 kSeasonMaskBytesPerPixel = 2;   // RG8
constexpr uint64 kTransmissionBytesPerPixel = 4; // RGBA8

bool IsRecent(uint64 requestFrame, uint64 frame)
{
    return requestFrame >= frame || frame - requestFrame <= 1u;
}

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
    const uint32 mipCount = ImpostorAtlasMipCount(key);
    uint64 pixels = 0;
    for (uint32 mip = 0; mip < mipCount; ++mip)
    {
        const uint64 tile = key.FrameSize >> mip;
        pixels += tile * tile;
    }
    const uint64 transmissionBytes = key.BakeDiffuseTransmission ? kTransmissionBytesPerPixel : 0u;
    return frames * pixels *
           (kColorBytesPerPixel + kNormalDepthBytesPerPixel + kSeasonMaskBytesPerPixel + transmissionBytes);
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

bool ImpostorAtlasCache::HasRecentPendingRequest(const ImpostorAtlasKey& key, uint64 frame) const
{
    const Entry* entry = m_Entries.Find(key);
    return entry && entry->Status == State::Pending && IsRecent(entry->LastUsedFrame, frame);
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
    const uint64 available = m_BudgetBytes - m_UsedBytes;
    if (bytes > available && !TryEvictLeastRecentlyUsed(bytes - available, frame, outEvicted))
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

void ImpostorAtlasCache::ReleasePending(const ImpostorAtlasKey& key)
{
    if (GetState(key) != State::Pending)
        return;
    m_UsedBytes -= ImpostorAtlasBytes(key);
    m_Entries.Erase(key);
}

void ImpostorAtlasCache::Clear()
{
    m_Entries.Clear();
    m_UsedBytes = 0;
}

bool ImpostorAtlasCache::TryEvictLeastRecentlyUsed(uint64 neededBytes, uint64 frame,
                                                 std::vector<ImpostorAtlasKey>& outEvicted)
{
    // Check the total first so a rejected request leaves the cache intact.
    // The previous frame's atlases are protected until callers refresh demand.
    uint64 reclaimable = 0;
    for (const auto& [key, entry] : m_Entries)
    {
        if (!IsRecent(entry.LastUsedFrame, frame))
            reclaimable += ImpostorAtlasBytes(key);
        if (reclaimable >= neededBytes)
            break;
    }
    if (reclaimable < neededBytes)
        return false;

    uint64 freed = 0;
    while (freed < neededBytes)
    {
        const ImpostorAtlasKey* oldest = nullptr;
        uint64 oldestFrame = std::numeric_limits<uint64>::max();
        for (const auto& [key, entry] : m_Entries)
        {
            if (!IsRecent(entry.LastUsedFrame, frame) && entry.LastUsedFrame < oldestFrame)
            {
                oldest = &key;
                oldestFrame = entry.LastUsedFrame;
            }
        }
        assert(oldest);
        const ImpostorAtlasKey victim = *oldest;
        const uint64 bytes = ImpostorAtlasBytes(victim);
        outEvicted.push_back(victim);
        freed += bytes;
        m_UsedBytes -= bytes;
        m_Entries.Erase(victim);
    }
    return true;
}

} // namespace GameEngine::Engine::Renderer
