#include "Engine/Rendering/VirtualShadows/VirtualShadowLookup.h"

#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

class VirtualShadowLookupTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Set.IsValid())
            m_Device->DestroyDescriptorSet(m_Set);
        if (m_Depth.IsValid())
            m_Device->DestroyTexture(m_Depth);
        if (m_ComparisonSampler.IsValid())
            m_Device->DestroySampler(m_ComparisonSampler);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        for (const auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
    }

    BufferHandle CreateBuffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        desc.debugName = "VirtualShadowLookupTest.Buffer";
        const auto handle = m_Device->CreateBuffer(desc);
        if (handle.IsValid())
            m_Buffers.push_back(handle);
        return handle;
    }

    void Upload(BufferHandle buffer, const void* source, size_t bytes)
    {
        void* mapped = m_Device->MapBuffer(buffer);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, source, bytes);
        m_Device->UnmapBuffer(buffer);
    }

    std::unique_ptr<IDevice> m_Device;
    TextureHandle m_Depth{};
    SamplerHandle m_Sampler{};
    SamplerHandle m_ComparisonSampler{};
    DescriptorSetHandle m_Set{};
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(VirtualShadowLookupTest, FiltersNonAdjacentLayersAndSignalsMissingPages)
{
    m_Device = CreateVulkanDeviceFast();
    if (!m_Device)
        GTEST_SKIP() << "No graphics device available";
    RecordProperty("graphics_api", static_cast<int>(m_Device->GetAPI()));

    ShaderPackage package{};
    std::string error;
    ASSERT_TRUE(LoadShaderPkg("Shaders/virtual_shadow_lookup_test.shaderpkg", m_Device->PreferredShaderSource(),
                              package, &error)) << error;
    auto shader = package.stageBytes.find("cs");
    ASSERT_NE(shader, package.stageBytes.end());

    constexpr std::array types{DescriptorType::CombinedImageSampler, DescriptorType::StorageBuffer,
                               DescriptorType::StorageBuffer, DescriptorType::StorageBuffer,
                               DescriptorType::CombinedImageSampler, DescriptorType::UniformBuffer};
    constexpr std::array bindings{0u, 1u, 2u, 60u, 61u, 8u};
    DescriptorSetLayoutDesc layout{};
    for (uint32_t i = 0; i < types.size(); ++i)
    {
        DescriptorBinding binding{};
        binding.binding = bindings[i];
        binding.type = types[i];
        binding.count = 1;
        binding.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(binding);
    }
    ComputePipelineDesc desc{};
    desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shader->second));
    desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    desc.DebugName = "VirtualShadowLookupTest";
    const auto pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
    ASSERT_TRUE(pipeline.IsValid());

    constexpr uint32_t kPageSide = 128;
    constexpr std::array depths{0.25f, 0.7f, 0.75f, 0.8f, 0.1f};
    TextureDesc texture{};
    texture.width = texture.height = kPageSide;
    texture.depth = texture.mipLevels = texture.sampleCount = 1;
    texture.arrayLayers = static_cast<uint32_t>(depths.size());
    // Populate real D32 attachments through depth clears and rasterization,
    // avoiding the generic Vulkan color-aspect buffer upload path.
    texture.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    texture.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::DepthStencil);
    texture.debugName = "VirtualShadowLookupTest.Depth";
    m_Depth = m_Device->CreateTexture(texture);
    m_Sampler = m_Device->CreateSampler(SamplerDesc::PointClamp("VirtualShadowLookupTest"));
    ASSERT_TRUE(m_Depth.IsValid());
    ASSERT_TRUE(m_Sampler.IsValid());
    m_ComparisonSampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("VirtualShadowLookupTest.Compare"));
    ASSERT_TRUE(m_ComparisonSampler.IsValid());

    constexpr uint32_t kMissing = std::numeric_limits<uint32_t>::max();
    constexpr std::array table{0u, 0u,
                               2u, 0u, 3u, 1u, 4u, // Complete 2x2, then coarse.
                               2u, kMissing, 3u, 1u, 4u, // Missing right fine page.
                               17u}; // Invalid physical layer.
    constexpr std::array expected{0.5f, 0.25f, 0.25f, -1.0f, 1.0f, 0.0f,
                                  0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 1.0f, 0.5625f, 0.0625f,
                                  0.0f, 1.0f, 1.0f, -1.0f, -1.0f, -1.0f, 0.0f, 0.0f, -1.0f, -1.0f,
                                  0.5625f, 0.0625f, 0.0f, -1.0f, -1.0f, -1.0f, 1.0f, -1.0f,
                                  0.25f, 0.5625f, 0.15625f, -1.0f, -1.0f};
    using namespace GameEngine::Engine::Renderer;
    struct Projections
    {
        VirtualShadowLookupHeaderGPU Header{};
        std::array<VirtualShadowLookupProjectionGPU, 2> Rows{};
    } projections;
    projections.Header.DirectionalRows = {0, 1, kMissing, kMissing};
    for (uint32_t row = 0; row < projections.Rows.size(); ++row)
    {
        auto& projection = projections.Rows[row];
        projection.RelativeToLightClip[0] = row == 0 ? 1.0f : 0.25f;
        projection.RelativeToLightClip[5] = projection.RelativeToLightClip[10] = projection.RelativeToLightClip[15] = 1.0f;
        projection.FirstPage = row == 0 ? 7 : 0;
        projection.PagesPerAxis = row == 0 ? 2 : 1;
        projection.LastMip = row == 0 ? 1 : 0;
        projection.LightPositionRelAndFar[0] = row == 0 ? 0.01f : 0.1f;
        projection.LightPositionRelAndFar[1] = 1.0f;
    }
    const auto metadata = CreateBuffer(sizeof(projections), BufferUsage::Storage, BufferMemoryUsage::Upload);
    ASSERT_TRUE(metadata.IsValid());
    ASSERT_NO_FATAL_FAILURE(Upload(metadata, &projections, sizeof(projections)));
    // Two levels, a depth bias of zero, the Poisson tap count and the reference level's metrics.
    ShadowDataGPU shadowData{};
    shadowData.shadowParams[2] = 2.0f;
    shadowData.shadowParams[3] = 100.0f;
    shadowData.shadowPcss[1] = 8.0f;
    shadowData.shadowPcssCascades[0][0] = 0.01f;
    shadowData.shadowPcssCascades[0][1] = 1.0f;
    const auto shadowConstants = CreateBuffer(sizeof(shadowData), BufferUsage::Uniform, BufferMemoryUsage::Upload);
    ASSERT_TRUE(shadowConstants.IsValid());
    ASSERT_NO_FATAL_FAILURE(Upload(shadowConstants, &shadowData, sizeof(shadowData)));
    const auto pageTable = CreateBuffer(sizeof(table), BufferUsage::Storage, BufferMemoryUsage::Upload);
    const auto output = CreateBuffer(sizeof(expected), BufferUsage::Storage, BufferMemoryUsage::Readback);
    ASSERT_TRUE(pageTable.IsValid() && output.IsValid());
    ASSERT_NO_FATAL_FAILURE(Upload(pageTable, table.data(), sizeof(table)));

    DescriptorSetDesc set{};
    set.layout = layout;
    m_Set = m_Device->CreateDescriptorSet(set);
    ASSERT_TRUE(m_Set.IsValid());
    m_Device->UpdateCombinedImageSamplerBinding(m_Set, 0, m_Depth, m_Sampler);
    m_Device->UpdateStorageBufferBinding(m_Set, 1, pageTable, 0, sizeof(table));
    m_Device->UpdateStorageBufferBinding(m_Set, 2, output, 0, sizeof(expected));
    m_Device->UpdateStorageBufferBinding(m_Set, 60, metadata, 0, sizeof(projections));
    m_Device->UpdateCombinedImageSamplerBinding(m_Set, 61, m_Depth, m_ComparisonSampler);
    m_Device->UpdateBufferBinding(m_Set, 8, shadowConstants, 0, sizeof(shadowData));

    ShaderPackage depthPackage{};
    ASSERT_TRUE(LoadShaderPkg("Shaders/virtual_shadow_test_depth.shaderpkg", m_Device->PreferredShaderSource(), depthPackage, &error)) << error;
    const auto vertex = depthPackage.stageBytes.find("vs");
    ASSERT_NE(vertex, depthPackage.stageBytes.end());
    GraphicsPipelineDesc depthDesc{};
    depthDesc.VertexShader = std::make_shared<const std::vector<uint8_t>>(vertex->second);
    depthDesc.Rasterization.cullMode = CullModeFlagBits::None;
    depthDesc.DepthStencil.depthTestEnable = depthDesc.DepthStencil.depthWriteEnable = true;
    depthDesc.DepthStencil.depthCompareOp = CompareOp::Always;
    depthDesc.DebugName = "VirtualShadowLookupTest.DepthPattern";
    PipelineFormatKey format{};
    format.DepthFormat = TextureFormat::D32_FLOAT;
    const auto depthPipeline = m_Device->GetOrCreateGraphicsPipeline(m_Device->InternGraphicsPipeline(depthDesc), format);
    ASSERT_TRUE(depthPipeline.IsValid());
    auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commands, nullptr);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(m_Depth, ResourceState::Undefined,
                                                            ResourceState::DepthWrite, 0, 1, 0, texture.arrayLayers));
    for (uint32_t layer = 0; layer < texture.arrayLayers; ++layer)
    {
        RenderPassDesc pass{};
        pass.depthTarget = m_Depth;
        pass.clearDepthValue = depths[layer];
        pass.useDepthView = true;
        pass.depthViewDesc.aspect = TextureAspect::Depth;
        pass.depthViewDesc.baseLayer = layer;
        pass.depthViewDesc.layerCount = 1;
        pass.depthViewDesc.levelCount = 1;
        commands->BeginRenderPass(pass);
        if (layer == 0)
        {
            commands->SetViewport(0, 0, 128, 128);
            commands->SetPipeline(depthPipeline);
            for (const auto& pixel : std::array<std::array<uint32_t, 2>, 3>{{{32,32},{33,32},{33,33}}})
            {
                commands->SetScissor(pixel[0], pixel[1], 1, 1);
                commands->Draw(3);
            }
        }
        commands->EndRenderPass();
    }
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(m_Depth, ResourceState::DepthWrite,
                                                            ResourceState::ShaderResource, 0, 1, 0, texture.arrayLayers));
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, m_Set, pipeline);
    commands->Dispatch(1, 1, 1);
    commands->End();
    m_Device->ExecuteCommandLists({commands.get()});
    m_Device->WaitForIdle();

    std::array<float, expected.size()> actual{};
    void* mapped = m_Device->MapBuffer(output);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(actual.data(), mapped, sizeof(actual));
    m_Device->UnmapBuffer(output);
    EXPECT_EQ(actual, expected);
}

} // namespace
