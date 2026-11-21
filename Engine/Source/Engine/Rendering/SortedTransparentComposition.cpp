#include "Engine/Rendering/SortedTransparentComposition.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>

namespace GameEngine::Engine::Renderer
{
namespace
{
struct PackedRecord
{
    uint64 Key;
    uint32 Index;
};

bool TryPackMeshSortKeys(std::span<const RenderSortKey> keys, std::span<const ECS::EntityHandle> entities,
                         std::pmr::vector<PackedRecord>& packed)
{
    if (keys.empty())
        return false;
    // Match depth, entity ID and input index once the other sort fields are uniform.
    const RenderSortKey& first = keys.front();
    constexpr uint32 kFloatSignBit = 0x80000000u;
    packed.reserve(keys.size());
    for (uint32 index = 0; index < keys.size(); ++index)
    {
        const auto& key = keys[index];
        if (key.Layer != first.Layer || key.Order != first.Order || key.Grouped ||
            key.MemberOrder != first.MemberOrder || key.MemberDepth != first.MemberDepth ||
            !std::isfinite(key.Depth) || std::abs(key.Depth) > std::numeric_limits<float>::max())
            return false;

        const float depth = static_cast<float>(key.Depth);
        if (static_cast<double>(depth) != key.Depth)
            return false;

        // The wide comparator treats positive and negative zero as equal.
        const uint32 bits = std::bit_cast<uint32>(depth == 0.0f ? 0.0f : depth);
        const uint32 sortable = (bits & kFloatSignBit) ? ~bits : bits ^ kFloatSignBit;
        packed.push_back({(uint64(~sortable) << 32) | entities[index].id, index});
    }
    return true;
}
} // namespace

void BuildTransparentComposition(
    std::span<const SortedTransparentRecord> records, std::span<const RenderSortKey> keys,
    std::span<const ECS::EntityHandle> entities, std::span<const SortedTransparentCommand> contributors,
    std::pmr::memory_resource& scratch, SortedTransparentRunPartition& partition,
    std::vector<uint32>& meshOrder, std::vector<TransparentCompositionSlot>& slots,
    bool directDraws)
{
    std::pmr::vector<uint32> order(&scratch);
    std::pmr::vector<PackedRecord> packed(&scratch);
    const bool usePacked = contributors.empty() && TryPackMeshSortKeys(keys, entities, packed);
    const uint32 meshCount = static_cast<uint32>(records.size());
    if (usePacked)
    {
        std::sort(packed.begin(), packed.end(), [](const PackedRecord& left, const PackedRecord& right)
        {
            return left.Key == right.Key ? left.Index < right.Index : left.Key < right.Key;
        });
    }
    else
    {
        order.resize(records.size() + contributors.size());
        std::iota(order.begin(), order.end(), 0u);
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
    }
    partition.Clear();
    meshOrder.clear();
    slots.clear();
    bool previousMesh = false;
    uint32 previousMaterial = 0;
    uint32 previousRecord = 0;
    const size_t orderSize = usePacked ? packed.size() : order.size();
    for (size_t position = 0; position < orderSize; ++position)
    {
        const uint32 index = usePacked ? packed[position].Index : order[position];
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
