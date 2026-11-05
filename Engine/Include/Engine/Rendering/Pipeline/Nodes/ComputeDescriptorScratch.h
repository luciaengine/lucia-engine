#pragma once

#include "Rendering/Core/Device.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes::Detail
{
/// Recording-thread scratch. Descriptor writes consume Updates() synchronously.
class ComputeDescriptorScratch final
{
  public:
    /// Starts a batch while retaining every resource array.
    void Reset()
    {
        m_Count = 0;
        m_Bindings.clear();
        m_BindingsSorted = false;
    }

    /// Returns a cleared slot; a later Add can invalidate the reference.
    Rendering::DescriptorSetUpdate& Add(uint32_t binding, Rendering::DescriptorType type)
    {
        if (m_Count == m_Updates.size())
            m_Updates.emplace_back();
        auto& update = m_Updates[m_Count++];
        m_Bindings.push_back(binding);
        m_BindingsSorted = false;
        update.binding = binding;
        update.arrayElement = 0;
        update.type = type;
        update.buffers.clear();
        update.textures.clear();
        update.textureViews.clear();
        update.samplers.clear();
        update.bufferOffsets.clear();
        update.bufferRanges.clear();
        update.accelerationStructures.clear();
        return update;
    }

    /// Active updates only; valid until the next Reset or Add.
    std::span<const Rendering::DescriptorSetUpdate> Updates() const
    {
        return {m_Updates.data(), m_Count};
    }

    /// Tests only the active batch, including sparse binding indices.
    bool ContainsBinding(uint32_t binding)
    {
        // Sort only the index: duplicate descriptor writes keep their original order.
        if (!m_BindingsSorted)
        {
            std::sort(m_Bindings.begin(), m_Bindings.end());
            m_BindingsSorted = true;
        }
        return std::binary_search(m_Bindings.begin(), m_Bindings.end(), binding);
    }

  private:
    std::vector<Rendering::DescriptorSetUpdate> m_Updates;
    size_t m_Count = 0;
    std::vector<uint32_t> m_Bindings;
    bool m_BindingsSorted = false;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes::Detail
