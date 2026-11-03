#include "MetalCommandList.h"

#include "MetalDevice.h"
#include "MetalDirectBindings.h"
#include "Logger/Logger.h"

namespace GameEngine::Rendering
{
void MetalCommandList::BindDirectDescriptorSet(uint32_t setIndex, MetalDescriptorSet& descriptors,
                                               const MetalPipeline& pipeline)
{
    const auto& layouts = pipeline.directSetLayouts;
    if (setIndex >= layouts.size()) return;
    for (const auto& binding : layouts[setIndex].Slots)
    {
        const auto* source = descriptors.layout.FindBinding(binding.Binding);
        if (source == nullptr) continue;
        for (uint32_t element = 0; element < binding.Count; ++element)
        {
            if (binding.BufferId != MetalArgumentSlot::kUnused)
            {
                const auto index = binding.BufferId + element;
                if (index >= kMetalDirectBufferSizesIndex)
                {
                    Logger::Log::Error("Metal direct bindings exceed the supported buffer table.");
                    return;
                }
                const auto slot = source->BufferId + element;
                auto* buffer = descriptors.directBuffers[slot];
                const auto offset = descriptors.directBufferOffsets[slot];
                m_DirectBufferSizes[index] = descriptors.directBufferSizes[slot];
                if (m_RenderEncoder != nullptr)
                {
                    m_RenderEncoder->setVertexBuffer(buffer, offset, index);
                    m_RenderEncoder->setFragmentBuffer(buffer, offset, index);
                }
                if (m_ComputeEncoder != nullptr) m_ComputeEncoder->setBuffer(buffer, offset, index);
            }
            if (binding.TextureId != MetalArgumentSlot::kUnused)
            {
                auto* texture = descriptors.directTextures[source->TextureId + element];
                const auto index = binding.TextureId + element;
                if (m_RenderEncoder != nullptr)
                {
                    m_RenderEncoder->setVertexTexture(texture, index);
                    m_RenderEncoder->setFragmentTexture(texture, index);
                }
                if (m_ComputeEncoder != nullptr) m_ComputeEncoder->setTexture(texture, index);
            }
            if (binding.SamplerId != MetalArgumentSlot::kUnused)
            {
                auto* sampler = descriptors.directSamplers[source->SamplerId + element];
                const auto index = binding.SamplerId + element;
                if (m_RenderEncoder != nullptr)
                {
                    m_RenderEncoder->setVertexSamplerState(sampler, index);
                    m_RenderEncoder->setFragmentSamplerState(sampler, index);
                }
                if (m_ComputeEncoder != nullptr) m_ComputeEncoder->setSamplerState(sampler, index);
            }
        }
    }
    if (m_RenderEncoder != nullptr)
    {
        m_RenderEncoder->setVertexBytes(m_DirectBufferSizes.data(), sizeof(m_DirectBufferSizes), kMetalDirectBufferSizesIndex);
        m_RenderEncoder->setFragmentBytes(m_DirectBufferSizes.data(), sizeof(m_DirectBufferSizes), kMetalDirectBufferSizesIndex);
    }
    if (m_ComputeEncoder != nullptr)
        m_ComputeEncoder->setBytes(m_DirectBufferSizes.data(), sizeof(m_DirectBufferSizes), kMetalDirectBufferSizesIndex);
}
}
