#pragma once
#include <memory_resource>

#include "Engine/Rendering/RenderSortKey.h"
#include "Engine/Rendering/SortedTransparentRuns.h"

namespace GameEngine::Engine::Renderer
{
/// Globally orders authored artwork, then splits runs at material and authored-order
/// boundaries. The GPU may depth-sort inside a run without crossing a layer or group.
/// GroupOfSlot refers to the original material run, which may appear in several slots.
inline void BuildAuthoredTransparentRunPartition(std::span<SortedTransparentRecord> records,
                                                 std::span<const RenderSortKey> keys,
                                                 std::pmr::memory_resource& scratch,
                                                 SortedTransparentRunPartition& partition)
{
    std::pmr::vector<uint32_t> order(records.size(), &scratch);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
              {
                  if (RenderSortBefore(keys[a], keys[b])) return true;
                  if (RenderSortBefore(keys[b], keys[a])) return false;
                  return a < b;
              });
    partition.SlotOfGroup.clear();
    partition.GroupOfSlot.clear();
    partition.RunStart.clear();
    partition.RunCount.clear();
    uint32_t previousRecord = 0;
    uint32_t previousMaterial = 0;
    for (uint32_t position = 0; position < order.size(); ++position)
    {
        const uint32_t index = order[position];
        const auto& key = keys[index];
        const auto& previous = keys[previousRecord];
        const uint32_t material = records[index].RunGroup;
        const bool startsRun = position == 0 || material != previousMaterial ||
            !std::isfinite(records[index].ViewDepth) || !std::isfinite(records[previousRecord].ViewDepth) ||
            key.Layer != previous.Layer || key.Order != previous.Order ||
            key.Grouped != previous.Grouped ||
            (key.Grouped && (key.Group != previous.Group || key.MemberOrder != previous.MemberOrder));
        if (startsRun)
        {
            partition.SlotOfGroup.push_back(static_cast<uint32_t>(partition.RunStart.size()));
            partition.GroupOfSlot.push_back(material);
            partition.RunStart.push_back(position);
            partition.RunCount.push_back(0);
        }
        records[index].RunGroup = static_cast<uint32_t>(partition.RunStart.size() - 1);
        ++partition.RunCount.back();
        previousRecord = index;
        previousMaterial = material;
    }
}
}
