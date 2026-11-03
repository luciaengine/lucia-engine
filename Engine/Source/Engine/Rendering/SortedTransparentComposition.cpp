#include "Engine/Rendering/SortedTransparentComposition.h"

#include <algorithm>
#include <numeric>

namespace GameEngine::Engine::Renderer
{
void BuildTransparentComposition(
    std::span<const SortedTransparentRecord> records, std::span<const RenderSortKey> keys,
    std::span<const ECS::EntityHandle> entities, std::span<const SortedTransparentCommand> contributors,
    std::pmr::memory_resource& scratch, SortedTransparentRunPartition& partition,
    std::vector<uint32>& meshOrder, std::vector<TransparentCompositionSlot>& slots,
    bool directDraws)
{
    std::pmr::vector<uint32> order(records.size() + contributors.size(), &scratch);
    std::iota(order.begin(), order.end(), 0u);
    const uint32 meshCount = static_cast<uint32>(records.size());
    const auto keyAt = [&](uint32 index) -> const RenderSortKey&
    { return index < meshCount ? keys[index] : contributors[index - meshCount].Sorting; };
    const auto entityAt = [&](uint32 index)
    { return index < meshCount ? entities[index] : contributors[index - meshCount].Entity; };
    std::sort(order.begin(), order.end(), [&](uint32 a, uint32 b)
    {
        if (RenderSortBefore(keyAt(a), keyAt(b))) return true;
        if (RenderSortBefore(keyAt(b), keyAt(a))) return false;
        const StringId familyA = a < meshCount ? 0 : contributors[a - meshCount].Family;
        const StringId familyB = b < meshCount ? 0 : contributors[b - meshCount].Family;
        if (familyA != familyB) return familyA < familyB;
        const auto entityA = entityAt(a);
        const auto entityB = entityAt(b);
        if (entityA.id != entityB.id) return entityA.id < entityB.id;
        const uint32 subA = a < meshCount ? 0u : contributors[a - meshCount].SubOrder;
        const uint32 subB = b < meshCount ? 0u : contributors[b - meshCount].SubOrder;
        if (subA != subB) return subA < subB;
        return a < b;
    });
    partition.Clear();
    meshOrder.clear();
    slots.clear();
    bool previousMesh = false;
    uint32 previousMaterial = 0;
    uint32 previousRecord = 0;
    for (const uint32 index : order)
    {
        if (index >= meshCount)
        {
            slots.push_back({index - meshCount, true});
            previousMesh = false;
            continue;
        }
        const uint32 material = records[index].RunGroup;
        const bool differentGeometry = directDraws && previousMesh &&
            (records[previousRecord].IndexCount != records[index].IndexCount ||
             records[previousRecord].FirstIndex != records[index].FirstIndex ||
             records[previousRecord].VertexOffset != records[index].VertexOffset);
        if (!previousMesh || previousMaterial != material || differentGeometry)
        {
            const uint32 slot = static_cast<uint32>(partition.RunStart.size());
            partition.SlotOfGroup.push_back(slot);
            partition.GroupOfSlot.push_back(material);
            partition.RunStart.push_back(static_cast<uint32>(meshOrder.size()));
            partition.RunCount.push_back(0);
            slots.push_back({slot, false});
        }
        meshOrder.push_back(index);
        ++partition.RunCount.back();
        previousMesh = true;
        previousMaterial = material;
        previousRecord = index;
    }
}
} // namespace GameEngine::Engine::Renderer
