/**
 * @file BatchRegistry.h
 * @brief Live-instance count per (materialIndex, meshIndex) draw batch.
 *
 * Maintained incrementally by GPUScene's three instance mutators — the only
 * places an instance's batch membership can change (extraction, particles,
 * and Clear/Release all route through them). The counts feed the R1.3/R1.4
 * scatter bucketer: the sorted key set becomes the GPU batch table and the
 * counts become exact per-batch output-region capacities, which is what lets
 * draw offsets be CPU-known while draw counts stay GPU-written.
 *
 * A mesh linked to an impostor card (SetMeshCardRow) also counts each of its
 * instances in the card's row, so the scatter can move an instance to its card.
 *
 * Not a hot path: entries mutate only when an instance's batch membership
 * changes (spawn/despawn/mesh-or-material swap), not per frame.
 */
#pragma once

#include "Types/FlatMap.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_map>

namespace GameEngine
{
namespace Rendering
{

class BatchRegistry
{
  public:
    /// Live-instance count for one (materialIndex, meshIndex) batch, split by
    /// winding parity so the scatter table builders can emit a parity-1 sibling
    /// row for mirrored (negative-determinant) instances. Even = parity-0
    /// (non-mirrored); Odd = parity-1 (mirrored). The pre-parity total is
    /// exactly Even + Odd — parity partitions the count, never adds to it (the
    /// conservation invariant the region prefix-sum depends on).
    struct ParityCounts
    {
        uint32_t Even = 0;
        uint32_t Odd  = 0;
        uint32_t Total() const { return Even + Odd; }
        bool operator==(const ParityCounts&) const = default;
    };

    /// 24-bit fields, matching GPUDrawStreamBuilder::MakeStreamKey's caps.
    /// Sort order of the packed key == (materialIndex, meshIndex) lexicographic,
    /// the same order WorldDrawBuilder sorts its per-view batch keys.
    static constexpr uint64_t MakeKey(uint32_t materialIndex, uint32_t meshIndex)
    {
        return (static_cast<uint64_t>(materialIndex & 0xFFFFFFu) << 24)
             | static_cast<uint64_t>(meshIndex & 0xFFFFFFu);
    }

    static constexpr uint32_t MaterialIndexFromKey(uint64_t key)
    {
        return static_cast<uint32_t>((key >> 24) & 0xFFFFFFu);
    }

    static constexpr uint32_t MeshIndexFromKey(uint64_t key)
    {
        return static_cast<uint32_t>(key & 0xFFFFFFu);
    }

    /// Tombstoned / cleared instances carry 0xFFFFFFFF mesh/material and must
    /// never enter the registry: an unguarded decrement on the tombstone key
    /// underflows to ~4·10⁹ and the scatter prefix sum would demand a region
    /// that large. Guarded on BOTH the increment and decrement side.
    static constexpr bool IsTracked(uint32_t materialIndex, uint32_t meshIndex)
    {
        return materialIndex != 0xFFFFFFFFu && meshIndex != 0xFFFFFFFFu;
    }

    void OnInstanceAdded(uint32_t materialIndex, uint32_t meshIndex, bool mirrored)
    {
        if (!IsTracked(materialIndex, meshIndex))
            return;
        AddCount(MakeKey(materialIndex, meshIndex), mirrored);
        if (const uint64_t* cardKey = m_CardRows.Find(meshIndex))
            AddCount(*cardKey, mirrored);
    }

    void OnInstanceRemoved(uint32_t materialIndex, uint32_t meshIndex, bool mirrored)
    {
        if (!IsTracked(materialIndex, meshIndex))
            return;
        RemoveCount(MakeKey(materialIndex, meshIndex), mirrored);
        if (const uint64_t* cardKey = m_CardRows.Find(meshIndex))
            RemoveCount(*cardKey, mirrored);
    }

    void OnInstanceUpdated(uint32_t oldMaterialIndex, uint32_t oldMeshIndex, bool oldMirrored,
                           uint32_t newMaterialIndex, uint32_t newMeshIndex, bool newMirrored)
    {
        if (oldMaterialIndex == newMaterialIndex && oldMeshIndex == newMeshIndex
            && oldMirrored == newMirrored)
            return;
        OnInstanceRemoved(oldMaterialIndex, oldMeshIndex, oldMirrored);
        OnInstanceAdded(newMaterialIndex, newMeshIndex, newMirrored);
    }

    /// Key → per-parity live instance counts. Keys iterate unordered; snapshot
    /// consumers sort by key (== (materialIndex, meshIndex)) before
    /// prefix-summing, and split each batch into even/odd sibling rows.
    const std::unordered_map<uint64_t, ParityCounts>& Counts() const { return m_Counts; }

    /// The synthetic card batch key for each linked primary mesh.
    const FlatMap<uint32_t, uint64_t>& CardRows() const { return m_CardRows; }

    size_t BatchCount() const { return m_Counts.size(); }

    /// Links the mesh to an impostor card row: each instance of the mesh also counts in the row
    /// (cardMaterialIndex, cardMeshIndex), so the row has room for every instance that can switch
    /// to its card. Only the primary part of a linked model has a card row. A second call moves
    /// the counts to the new row.
    void SetMeshCardRow(uint32_t meshIndex, uint32_t cardMaterialIndex, uint32_t cardMeshIndex)
    {
        ClearMeshCardRow(meshIndex);
        if (!IsTracked(cardMaterialIndex, cardMeshIndex))
            return;
        const uint64_t cardKey = MakeKey(cardMaterialIndex, cardMeshIndex);
        const ParityCounts counts = MeshCounts(meshIndex);
        AddCounts(cardKey, counts);
        m_CardRows.InsertOrAssign(meshIndex, cardKey);
    }

    /// Removes the card row of the mesh and its counts.
    void ClearMeshCardRow(uint32_t meshIndex)
    {
        const uint64_t* cardKey = m_CardRows.Find(meshIndex);
        if (!cardKey)
            return;
        RemoveCounts(*cardKey, MeshCounts(meshIndex));
        m_CardRows.Erase(meshIndex);
    }

    void Clear()
    {
        m_Counts.clear();
        m_CardRows.Clear();
    }

  private:
    void AddCount(uint64_t key, bool mirrored)
    {
        ParityCounts& c = m_Counts[key];
        if (mirrored)
            ++c.Odd;
        else
            ++c.Even;
    }

    void AddCounts(uint64_t key, const ParityCounts& counts)
    {
        if (counts.Total() == 0u)
            return;
        ParityCounts& c = m_Counts[key];
        c.Even += counts.Even;
        c.Odd += counts.Odd;
    }

    void RemoveCount(uint64_t key, bool mirrored)
    {
        auto it = m_Counts.find(key);
        // A miss (or an empty parity bucket) here means a count-drift bug
        // upstream; tolerate in Release (skip) so the failure mode is a
        // conservative over-allocation, never an underflow.
        assert(it != m_Counts.end() && (mirrored ? it->second.Odd : it->second.Even) > 0u);
        if (it == m_Counts.end())
            return;
        uint32_t& lane = mirrored ? it->second.Odd : it->second.Even;
        if (lane > 0u)
            --lane;
        if (it->second.Total() == 0u)
            m_Counts.erase(it);
    }

    void RemoveCounts(uint64_t key, const ParityCounts& counts)
    {
        auto it = m_Counts.find(key);
        assert(counts.Total() == 0u || (it != m_Counts.end() && it->second.Even >= counts.Even
                                        && it->second.Odd >= counts.Odd));
        if (it == m_Counts.end())
            return;
        it->second.Even -= std::min(it->second.Even, counts.Even);
        it->second.Odd -= std::min(it->second.Odd, counts.Odd);
        if (it->second.Total() == 0u)
            m_Counts.erase(it);
    }

    /// The live instance counts of the mesh over all its materials. Linear in the batch count:
    /// it runs only when a link changes.
    ParityCounts MeshCounts(uint32_t meshIndex) const
    {
        ParityCounts sum{};
        for (const auto& [key, counts] : m_Counts)
        {
            if (MeshIndexFromKey(key) != (meshIndex & 0xFFFFFFu))
                continue;
            sum.Even += counts.Even;
            sum.Odd += counts.Odd;
        }
        return sum;
    }

    std::unordered_map<uint64_t, ParityCounts> m_Counts;
    // Mesh index -> card row key of the primary part of a linked model.
    FlatMap<uint32_t, uint64_t> m_CardRows;
};

} // namespace Rendering
} // namespace GameEngine
