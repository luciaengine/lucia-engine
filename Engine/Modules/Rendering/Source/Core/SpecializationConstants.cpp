/**
 * @file SpecializationConstants.cpp
 * @brief Enhanced Specialization Constants implementation
 */

#include "Rendering/Core/SpecializationConstants.h"
#include "Rendering/Core/HashUtils.h"
#include <iostream>
#include <algorithm>
#include <functional>

namespace GameEngine {
namespace Rendering {

    namespace
    {
        uint64_t HashSpecializationEntry(uint64_t hash, uint32_t constantId,
                                         const SpecializationConstants::Entry& entry, const uint8_t* data)
        {
            hash = HashUtils::HashCombine(hash, constantId);
            hash = HashUtils::HashCombine(hash, static_cast<uint32_t>(entry.Size));
            for (size_t i = 0; i < entry.Size; ++i)
                hash = HashUtils::HashCombine(hash, data[entry.Offset + i]);
            return hash;
        }
    }

    void SpecializationConstants::Merge(const SpecializationConstants& overrides)
    {
        if (this == &overrides)
            return;
        for (const auto& [id, source] : overrides.m_Entries)
        {
            Entry entry = source;
            entry.Offset = static_cast<uint32_t>(m_Data.size());
            m_Data.insert(m_Data.end(), overrides.m_Data.begin() + source.Offset,
                          overrides.m_Data.begin() + source.Offset + source.Size);
            m_Entries[id] = std::move(entry);
        }
    }

    uint64_t SpecializationConstants::GetHash() const {
        if (m_Entries.size() == 1)
        {
            const auto& [constantId, entry] = *m_Entries.begin();
            return HashSpecializationEntry(0, constantId, entry, m_Data.data());
        }

        // Walk constants in sorted-by-id order so two SpecializationConstants
        // with identical content but different insertion order produce the
        // same hash. Required for the pipeline cache: two call sites that
        // build the same constants in different orders must intern to the
        // same id, otherwise the cache fragments per call site.
        std::vector<uint32_t> sortedIds;
        sortedIds.reserve(m_Entries.size());
        for (const auto& [constantId, entry] : m_Entries) {
            (void)entry;
            sortedIds.push_back(constantId);
        }
        std::sort(sortedIds.begin(), sortedIds.end());

        uint64_t hash = 0;
        for (uint32_t constantId : sortedIds)
            hash = HashSpecializationEntry(hash, constantId, m_Entries.at(constantId), m_Data.data());
        return hash;
    }

} // namespace Rendering
} // namespace GameEngine
