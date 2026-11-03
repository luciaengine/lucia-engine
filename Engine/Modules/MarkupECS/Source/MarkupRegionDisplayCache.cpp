#include "MarkupECS/MarkupRegionDisplayCache.h"

#include <vector>

namespace GameEngine::MarkupECS
{

const MarkupRegionDisplayCache::Entry& MarkupRegionDisplayCache::Resolve(ECS::EntityHandle region,
                                                                          const MarkupRegionGroundKey& key,
                                                                          float32 extrudeHeight, uint64 frame,
                                                                          const GroundBuilder& buildGround)
{
    std::unique_ptr<Entry>& slot = m_Entries.GetOrInsert(region.id);
    const bool fresh = !slot;
    if (fresh)
        slot = std::make_unique<Entry>();
    Entry& entry = *slot;
    const bool groundChanged = fresh || !(entry.GroundKey == key);
    if (groundChanged)
    {
        entry.GroundKey = key;
        entry.Ground = buildGround();
        ++m_GroundBuilds;
    }
    if (groundChanged || entry.ExtrudeHeight != extrudeHeight)
    {
        entry.ExtrudeHeight = extrudeHeight;
        entry.Mesh = BuildMarkupRegionMesh(entry.Ground, extrudeHeight);
        entry.MeshRevision = m_NextMeshRevision++;
        entry.ChangedFrame = frame;
        ++m_MeshBuilds;
    }
    entry.LastSeenFrame = frame;
    return entry;
}

const MarkupRegionDisplayCache::Entry* MarkupRegionDisplayCache::Find(ECS::EntityHandle region) const
{
    const std::unique_ptr<Entry>* slot = m_Entries.Find(region.id);
    return slot ? slot->get() : nullptr;
}

void MarkupRegionDisplayCache::EvictUnseen(uint64 frame)
{
    m_Entries.EraseIf([frame](uint32, const std::unique_ptr<Entry>& entry) {
        return frame > entry->LastSeenFrame + kEvictAfterFrames;
    });
}

void MarkupRegionDisplayCache::Clear()
{
    m_Entries.Clear();
}

} // namespace GameEngine::MarkupECS
