#pragma once

#include "CBTTerrain/CBTLayout.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

namespace GameEngine::TerrainECS::Test
{

/// Runs the shipped paged-height resolve over a supplied cache and packed page table.
class TerrainPageGpuProbe
{
public:
    struct QueryPoint
    {
        float32 U;
        float32 V;
        float32 Lambda;
        float32 Unused = 0;
    };

    explicit TerrainPageGpuProbe(Rendering::IDevice& device) : m_Device(device) {}
    ~TerrainPageGpuProbe()
    {
        for (Rendering::BufferHandle buffer : m_Buffers)
            m_Device.DestroyBuffer(buffer);
        if (m_Sampler.IsValid())
            m_Device.DestroySampler(m_Sampler);
    }

    bool Initialize()
    {
        std::ifstream file(TERRAIN_PAGE_PROBE_SPV, std::ios::binary);
        const std::vector<uint8> spirv{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (spirv.empty())
            return false;
        m_Layout.debugName = "TerrainPageResolveProbe.SetLayout";
        for (uint32 binding = 0; binding < 6; ++binding)
        {
            Rendering::DescriptorBinding entry{};
            entry.binding = binding;
            entry.type = binding == 0 ? Rendering::DescriptorType::CombinedImageSampler : Rendering::DescriptorType::StorageBuffer;
            entry.count = 1;
            entry.shaderStages = Rendering::kShaderStageCompute;
            m_Layout.bindings.push_back(entry);
        }
        Rendering::ComputePipelineDesc description{};
        description.ComputeShader = std::make_shared<const std::vector<uint8>>(spirv);
        description.DescriptorSetLayouts.push_back(m_Device.InternDescriptorSetLayout(m_Layout));
        description.DebugName = "TerrainPageResolveProbe";
        m_Pipeline = m_Device.InternComputePipeline(description);
        m_Sampler = m_Device.CreateSampler(Rendering::SamplerDesc::MaterialLinearClamp("TerrainPageResolveProbe.LinearClamp"));
        return m_Device.GetOrCreateComputePipeline(m_Pipeline).IsValid() && m_Sampler.IsValid();
    }

    Rendering::BufferHandle Declare(Rendering::RenderGraph::RGFrame& frame, Rendering::TextureHandle cache,
                                    std::span<const uint32> table, std::span<const QueryPoint> points)
    {
        namespace Graph = Rendering::RenderGraph;
        const auto entries = table.subspan(CBTTerrain::kCBTPageFadeOffsetWords + table[4]);
        const auto fades = table.subspan(CBTTerrain::kCBTPageFadeOffsetWords, table[4]);
        const auto levels = table.subspan(CBTTerrain::kCBTPageHeaderWords, 4u * table[2]);
        std::vector<uint32> queries(8u + 4u * points.size(), 0);
        std::copy_n(table.begin(), 4, queries.begin());
        queries[4] = static_cast<uint32>(points.size());
        std::memcpy(queries.data() + 8, points.data(), points.size_bytes());
        const auto entryBuffer = MakeBuffer(entries.data(), entries.size_bytes());
        const auto fadeBuffer = MakeBuffer(fades.data(), fades.size_bytes());
        const auto levelBuffer = MakeBuffer(levels.data(), levels.size_bytes());
        const auto queryBuffer = MakeBuffer(queries.data(), queries.size() * sizeof(uint32));
        const auto results = MakeBuffer(nullptr, points.size() * sizeof(float32));
        Rendering::DescriptorSetDesc setDescription{};
        setDescription.layout = m_Layout;
        setDescription.transient = true;
        setDescription.debugName = "TerrainPageResolveProbe.Set";
        const auto set = m_Device.CreateDescriptorSet(setDescription);
        m_Device.UpdateCombinedImageSamplerBinding(set, 0, cache, m_Sampler);
        m_Device.UpdateStorageBufferBinding(set, 1, entryBuffer, 0, entries.size_bytes());
        m_Device.UpdateStorageBufferBinding(set, 2, fadeBuffer, 0, fades.size_bytes());
        m_Device.UpdateStorageBufferBinding(set, 3, levelBuffer, 0, levels.size_bytes());
        m_Device.UpdateStorageBufferBinding(set, 4, queryBuffer, 0, queries.size() * sizeof(uint32));
        m_Device.UpdateStorageBufferBinding(set, 5, results, 0, points.size() * sizeof(float32));
        const auto texture = frame.ImportExternalTexture("TerrainPageResolveProbe.Cache", cache, Rendering::ResourceState::ShaderResource);
        const auto output = frame.ImportExternalBuffer("TerrainPageResolveProbe.Results", results, points.size() * sizeof(float32));
        const auto pipeline = m_Pipeline;
        const auto groups = static_cast<uint32>((points.size() + 63u) / 64u);
        frame.AddPass("TerrainPageResolveProbe", 0,
            [texture, output, pipeline](Graph::RGPassBuilder& pass)
            {
                pass.Read(texture, Graph::RGTextureRead::SampledCompute);
                pass.Write(output, Graph::RGBufferWrite::Storage);
                pass.UsePipeline(pipeline);
            },
            [pipeline, set, groups](Graph::RGContext& context)
            {
                context.Cmd->SetPipeline(context.GetOrCreatePipelineVariant(pipeline));
                context.Cmd->BindDescriptorSet(0, set, context.GetOrCreatePipelineVariant(pipeline));
                context.Cmd->Dispatch(groups, 1, 1);
            });
        frame.MarkOutput(output);
        return results;
    }

    std::vector<float32> Read(Rendering::BufferHandle buffer, std::size_t count)
    {
        std::vector<float32> heights(count);
        const void* mapped = m_Device.MapBuffer(buffer);
        if (mapped != nullptr)
            std::memcpy(heights.data(), mapped, count * sizeof(float32));
        m_Device.UnmapBuffer(buffer);
        return heights;
    }

private:
    Rendering::BufferHandle MakeBuffer(const void* data, std::size_t bytes)
    {
        Rendering::BufferDesc description{};
        description.size = std::max<std::size_t>(bytes, 16);
        description.usage = static_cast<uint32>(Rendering::BufferUsage::Storage) | static_cast<uint32>(Rendering::BufferUsage::TransferDst);
        description.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        description.debugName = "TerrainPageResolveProbe.Buffer";
        const auto buffer = m_Device.CreateBuffer(description);
        m_Buffers.push_back(buffer);
        if (data != nullptr)
            m_Device.UpdateBuffer(buffer, 0, bytes, data);
        return buffer;
    }

    Rendering::IDevice& m_Device;
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::ComputePipelineId m_Pipeline{};
    Rendering::SamplerHandle m_Sampler{};
    std::vector<Rendering::BufferHandle> m_Buffers;
};

} // namespace GameEngine::TerrainECS::Test
