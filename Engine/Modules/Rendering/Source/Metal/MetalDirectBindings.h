#pragma once

#include "MetalArgumentBufferLayout.h"

#include <vector>

namespace GameEngine::Rendering
{
// Direct Metal resources have independent buffer, texture and sampler tables.
// Assign across every set so separate descriptor sets cannot alias a table slot.
inline std::vector<MetalArgumentBufferLayout> ComputeMetalDirectBindings(
    const std::vector<MetalArgumentBufferLayout>& layouts)
{
    auto result = layouts;
    uint32_t buffer = 0, texture = 0, sampler = 0;
    for (auto& layout : result)
    {
        for (auto& slot : layout.Slots)
        {
            if (slot.BufferId != MetalArgumentSlot::kUnused) { slot.BufferId = buffer; buffer += slot.Count; }
            if (slot.TextureId != MetalArgumentSlot::kUnused) { slot.TextureId = texture; texture += slot.Count; }
            if (slot.SamplerId != MetalArgumentSlot::kUnused) { slot.SamplerId = sampler; sampler += slot.Count; }
        }
    }
    return result;
}
inline constexpr uint32_t kMetalDirectBufferSizesIndex = 25;
}
