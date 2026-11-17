#include "Engine/Rendering/VirtualShadows/VirtualShadowLookup.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowRequestReuse.h"

#include "Engine/Rendering/VirtualShadows/VirtualShadowPageMark.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::VirtualShadowMarkParamsGPU;
using GameEngine::Engine::Renderer::VirtualShadowMarkProjectionGPU;

namespace
{

class VirtualShadowPageMarkTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Set.IsValid())
            m_Device->DestroyDescriptorSet(m_Set);
        if (m_LookupSet.IsValid())
            m_Device->DestroyDescriptorSet(m_LookupSet);
        if (m_LookupDepth.IsValid())
            m_Device->DestroyTexture(m_LookupDepth);
        if (m_Depth.IsValid())
            m_Device->DestroyTexture(m_Depth);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        if (m_ComparisonSampler.IsValid())
            m_Device->DestroySampler(m_ComparisonSampler);
        for (BufferHandle buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
    }

    BufferHandle MakeBuffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        desc.debugName = "VirtualShadowPageMarkTest.Buffer";
        const BufferHandle buffer = m_Device->CreateBuffer(desc);
        EXPECT_TRUE(buffer.IsValid());
        if (buffer.IsValid())
            m_Buffers.push_back(buffer);
        return buffer;
    }

    void Upload(BufferHandle buffer, const void* bytes, size_t size)
    {
        void* mapped = m_Device->MapBuffer(buffer);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, bytes, size);
        m_Device->UnmapBuffer(buffer);
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetHandle m_Set{};
    TextureHandle m_Depth{};
    TextureHandle m_LookupDepth{};
    DescriptorSetHandle m_LookupSet{};
    SamplerHandle m_Sampler{};
    SamplerHandle m_ComparisonSampler{};
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(VirtualShadowPageMarkTest, MarksSunPageEdgesSpotMipsAndBothPointFaces)
{
    m_Device = CreateVulkanDeviceFast();
    if (!m_Device)
        GTEST_SKIP() << "No graphics device available";
    RecordProperty("graphics_api", static_cast<int>(m_Device->GetAPI()));

    ShaderPackage pkg{};
    std::string error;
    ASSERT_TRUE(LoadShaderPkg("Shaders/virtual_shadow_mark.shaderpkg", m_Device->PreferredShaderSource(), pkg, &error))
        << error;
    auto shader = pkg.stageBytes.find("cs");
    ASSERT_NE(shader, pkg.stageBytes.end());

    constexpr std::array types{DescriptorType::CombinedImageSampler, DescriptorType::StorageBuffer,
                               DescriptorType::StorageBuffer, DescriptorType::UniformBuffer};
    DescriptorSetLayoutDesc layout{};
    for (uint32_t binding = 0; binding < types.size(); ++binding)
    {
        DescriptorBinding descriptor{};
        descriptor.binding = binding;
        descriptor.type = types[binding];
        descriptor.count = 1;
        descriptor.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(descriptor);
    }
    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shader->second));
    pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    pipelineDesc.DebugName = "VirtualShadowPageMarkTest";
    const auto pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
    ASSERT_TRUE(pipeline.IsValid());

    constexpr uint32_t kWidth = 512;
    constexpr uint32_t kHeight = 513;
    constexpr uint32_t kTextureExtent = kHeight + 16;
    constexpr uint32_t kViewportX = 9;
    constexpr uint32_t kViewportY = 5;
    std::vector<float> depth(kTextureExtent * kTextureExtent, 0.0f);
    depth[(kViewportY + 256) * kTextureExtent + kViewportX + 255] = 0.5f;
    depth[0] = 1.0f; // Outside the viewport, even though it has non-sky depth.
    const size_t depthBytes = depth.size() * sizeof(float);

    // Pack 16 fine pages, four intermediate pages and one coarsest page
    // per projection, deliberately sharing destination words across groups.
    constexpr uint32_t kProjections = 7;
    constexpr uint32_t kFirstPage = 23;
    constexpr uint32_t kPagesPerProjection = 21;
    constexpr uint32_t kWords = (kFirstPage + kProjections * kPagesPerProjection + 31) / 32;
    std::array<VirtualShadowMarkProjectionGPU, kProjections> projections{};
    for (uint32_t i = 0; i < kProjections; ++i)
    {
        auto& p = projections[i];
        p.ViewClipToLightClip.Data()[0] = p.ViewClipToLightClip.Data()[5] = 1.0f;
        p.ViewClipToLightClip.Data()[10] = p.ViewClipToLightClip.Data()[15] = 1.0f;
        p.FirstPage = kFirstPage + i * kPagesPerProjection;
        p.PagesPerAxis = 4;
        p.LastMip = 2;
    }
    projections[0].LastMip = 0;
    projections[0].ViewClipToLightClip.Data()[13] = 0.5f;
    projections[2].ViewClipToLightClip.Data()[12] = 1.0f;
    projections[3].ViewClipToLightClip.Data()[12] = -1.0f;
    projections[2].FilterRadiusUV = projections[3].FilterRadiusUV = 1.5f / 1024.0f;
    projections[4].ViewClipToLightClip.Data()[12] = 4.0f;
    projections[5].ViewClipToLightClip.Data()[0] = 4.0f;
    // This light-space X depends on receiver depth. The receiver compute
    // witness supplies analytic helper-lane gradients for mip selection.
    projections[6].ViewClipToLightClip.Data()[8] = 2.0f;
    projections[6].ViewClipToLightClip.Data()[12] = -1.02f;
    projections[6].NormalBiasClipBounds[0] = 0.04472136f;
    projections[6].NormalBiasClipBounds[1] = projections[6].NormalBiasClipBounds[2] = 0.02f;
    const VirtualShadowMarkParamsGPU params{kWidth, kHeight, kProjections, kWords, kViewportX, kViewportY, {}, {0.0f, 0.0f, 0.0f, 1.0f}};

    TextureDesc td{};
    td.width = td.height = kTextureExtent;
    td.depth = td.mipLevels = td.sampleCount = td.arrayLayers = 1;
    // The upload API copies the color aspect. The sampled float values are
    // identical to what the shader reads from resolved view depth.
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    td.debugName = "VirtualShadowPageMarkTest.Depth";
    m_Depth = m_Device->CreateTexture(td);
    m_Sampler = m_Device->CreateSampler(SamplerDesc::PointClamp("VirtualShadowPageMarkTest"));
    ASSERT_TRUE(m_Depth.IsValid());
    ASSERT_TRUE(m_Sampler.IsValid());

    using Words = std::array<uint32_t, kWords>;
    const auto upload = MakeBuffer(depthBytes, BufferUsage::TransferSrc, BufferMemoryUsage::Upload);
    const auto projectionBuffer = MakeBuffer(sizeof(projections), BufferUsage::Storage, BufferMemoryUsage::Upload);
    const auto paramsBuffer = MakeBuffer(sizeof(params), BufferUsage::Uniform, BufferMemoryUsage::Upload);
    const auto requests = MakeBuffer(sizeof(Words), BufferUsage::Storage | BufferUsage::TransferDst,
                                     BufferMemoryUsage::Readback);
    ASSERT_TRUE(upload.IsValid() && projectionBuffer.IsValid() && paramsBuffer.IsValid() && requests.IsValid());
    ASSERT_NO_FATAL_FAILURE(Upload(upload, depth.data(), depthBytes));
    ASSERT_NO_FATAL_FAILURE(Upload(projectionBuffer, projections.data(), sizeof(projections)));
    ASSERT_NO_FATAL_FAILURE(Upload(paramsBuffer, &params, sizeof(params)));

    DescriptorSetDesc ds{};
    ds.layout = layout;
    m_Set = m_Device->CreateDescriptorSet(ds);
    ASSERT_TRUE(m_Set.IsValid());
    m_Device->UpdateCombinedImageSamplerBinding(m_Set, 0, m_Depth, m_Sampler);
    m_Device->UpdateStorageBufferBinding(m_Set, 1, projectionBuffer, 0, sizeof(projections));
    m_Device->UpdateStorageBufferBinding(m_Set, 2, requests, 0, sizeof(Words));
    DescriptorSetUpdate uniform{};
    uniform.binding = 3;
    uniform.type = DescriptorType::UniformBuffer;
    uniform.buffers = {paramsBuffer};
    uniform.bufferOffsets = {0};
    uniform.bufferRanges = {sizeof(params)};
    m_Device->UpdateDescriptorSet(m_Set, uniform);

    auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commands, nullptr);
    commands->Begin();
    commands->CopyBufferToTextureSubresource(upload, m_Depth, 0, 0, kTextureExtent, kTextureExtent);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(m_Depth, ResourceState::CopyDest,
                                                            ResourceState::ShaderResource));
    commands->FillBuffer(requests, 0, sizeof(Words), 0);
    commands->Barrier(ResourceBarrier::CreateBufferBarrier(requests, ResourceState::CopyDest,
                                                           ResourceState::UnorderedAccess));
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, m_Set, pipeline);
    commands->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, kProjections);
    commands->End();
    m_Device->ExecuteCommandLists({commands.get()});
    m_Device->WaitForIdle();

    Words actual{};
    void* mapped = m_Device->MapBuffer(requests);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(actual.data(), mapped, sizeof(actual));
    m_Device->UnmapBuffer(requests);

    constexpr uint32_t kCoarsest = 1u << 20;
    constexpr uint32_t kParents = (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19) | kCoarsest;
    constexpr std::array<uint32_t, kProjections> projectionMasks{
        (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6),
        (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | kParents,
        (1u << 7) | (1u << 11) | (1u << 17) | (1u << 19) | kCoarsest,
        (1u << 4) | (1u << 8) | (1u << 16) | (1u << 18) | kCoarsest,
        0u,
        (1u << 5) | (1u << 9) | (1u << 16) | (1u << 18) | kCoarsest,
        (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | kParents,
    };
    Words expected{};
    for (uint32_t i = 0; i < kProjections; ++i)
    {
        for (uint32_t page = 0; page < kPagesPerProjection; ++page)
        {
            if ((projectionMasks[i] & (1u << page)) == 0)
                continue;
            const uint32_t address = projections[i].FirstPage + page;
            expected[address / 32] |= 1u << (address % 32);
        }
    }
    EXPECT_EQ(actual, expected);

    // Construct the physical lookup only from the marker's actual request
    // bits, then run the production mip and bilinear lookup on the GPU.
    using namespace GameEngine::Engine::Renderer;
    std::array<uint32_t, kProjections * kPagesPerProjection + kFirstPage> assignments;
    assignments.fill(0xffffffffu);
    for (uint32_t address = 0; address < assignments.size(); ++address)
        if ((actual[address / 32] & (1u << (address % 32))) != 0)
            assignments[address] = 0;
    struct Metadata
    {
        VirtualShadowLookupHeaderGPU Header{};
        VirtualShadowLookupProjectionGPU Projection{};
    } metadata;
    metadata.Projection.RelativeToLightClip = projections[6].ViewClipToLightClip;
    metadata.Projection.FirstPage = projections[6].FirstPage;
    metadata.Projection.PagesPerAxis = 4;
    metadata.Projection.LastMip = 2;
    const auto table = MakeBuffer(sizeof(assignments), BufferUsage::Storage, BufferMemoryUsage::Upload);
    const auto lookupMetadata = MakeBuffer(sizeof(metadata), BufferUsage::Storage, BufferMemoryUsage::Upload);
    constexpr std::array resultExpected{1.0f, 0.0f, 0.0f};
    const auto result = MakeBuffer(sizeof(resultExpected), BufferUsage::Storage, BufferMemoryUsage::Readback);
    ASSERT_TRUE(table.IsValid() && lookupMetadata.IsValid() && result.IsValid());
    ASSERT_NO_FATAL_FAILURE(Upload(table, assignments.data(), sizeof(assignments)));
    ASSERT_NO_FATAL_FAILURE(Upload(lookupMetadata, &metadata, sizeof(metadata)));
    TextureDesc lookupDepth = td;
    lookupDepth.width = lookupDepth.height = 128;
    lookupDepth.arrayLayers = 2;
    lookupDepth.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    lookupDepth.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::DepthStencil);
    lookupDepth.debugName = "VirtualShadowPageMarkTest.LookupDepth";
    m_LookupDepth = m_Device->CreateTexture(lookupDepth);
    ASSERT_TRUE(m_LookupDepth.IsValid());
    m_ComparisonSampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("VirtualShadowPageMarkTest.Compare"));
    ASSERT_TRUE(m_ComparisonSampler.IsValid());
    ShaderPackage lookupPackage{};
    ASSERT_TRUE(LoadShaderPkg("Shaders/virtual_shadow_receiver_test.shaderpkg", m_Device->PreferredShaderSource(), lookupPackage, &error)) << error;
    const auto lookupShader = lookupPackage.stageBytes.find("cs");
    ASSERT_NE(lookupShader, lookupPackage.stageBytes.end());
    DescriptorSetLayoutDesc lookupLayout{};
    for (uint32_t binding : {0u, 1u, 2u, 60u, 61u})
    {
        DescriptorBinding descriptor{};
        descriptor.binding = binding;
        descriptor.type = (binding == 0 || binding == 61) ? DescriptorType::CombinedImageSampler : DescriptorType::StorageBuffer;
        descriptor.count = 1;
        descriptor.shaderStages = kShaderStageCompute;
        lookupLayout.bindings.push_back(descriptor);
    }
    ComputePipelineDesc lookupPipelineDesc{};
    lookupPipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(lookupShader->second);
    lookupPipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(lookupLayout));
    lookupPipelineDesc.DebugName = "VirtualShadowPageMarkTest.Receiver";
    const auto lookupPipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(lookupPipelineDesc));
    ASSERT_TRUE(lookupPipeline.IsValid());
    DescriptorSetDesc lookupSet{};
    lookupSet.layout = lookupLayout;
    m_LookupSet = m_Device->CreateDescriptorSet(lookupSet);
    ASSERT_TRUE(m_LookupSet.IsValid());
    m_Device->UpdateCombinedImageSamplerBinding(m_LookupSet, 0, m_LookupDepth, m_Sampler);
    m_Device->UpdateCombinedImageSamplerBinding(m_LookupSet, 61, m_LookupDepth, m_ComparisonSampler);
    m_Device->UpdateStorageBufferBinding(m_LookupSet, 1, table, 0, sizeof(assignments));
    m_Device->UpdateStorageBufferBinding(m_LookupSet, 2, result, 0, sizeof(resultExpected));
    m_Device->UpdateStorageBufferBinding(m_LookupSet, 60, lookupMetadata, 0, sizeof(metadata));
    commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commands, nullptr);
    commands->Begin();
    // Populate real depth attachments. The generic buffer upload copies a
    // color aspect; depth clears also exercise the production compare sampler.
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(m_LookupDepth, ResourceState::Undefined,
                                                            ResourceState::DepthWrite, 0, 1, 0, 2));
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        RenderPassDesc pass{};
        pass.depthTarget = m_LookupDepth;
        pass.clearDepthValue = 0.75f;
        pass.useDepthView = true;
        pass.depthViewDesc.aspect = TextureAspect::Depth;
        pass.depthViewDesc.baseLayer = layer;
        pass.depthViewDesc.layerCount = 1;
        pass.depthViewDesc.levelCount = 1;
        commands->BeginRenderPass(pass);
        commands->EndRenderPass();
    }
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(m_LookupDepth, ResourceState::DepthWrite,
                                                            ResourceState::ShaderResource, 0, 1, 0, 2));
    commands->SetPipeline(lookupPipeline);
    commands->BindDescriptorSet(0, m_LookupSet, lookupPipeline);
    commands->Dispatch(1, 1, 1);
    commands->End();
    m_Device->ExecuteCommandLists({commands.get()});
    m_Device->WaitForIdle();
    std::array<float, resultExpected.size()> resultActual{};
    mapped = m_Device->MapBuffer(result);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(resultActual.data(), mapped, sizeof(resultActual));
    m_Device->UnmapBuffer(result);
    RecordProperty("receiver_mip", static_cast<int>(resultActual[0]));
    EXPECT_EQ(resultActual, resultExpected);

    // Exercise union retention with the production GPU marker. These are
    // prescribed raster-domain inputs, not a rasterizer or TAA resolve test.
    using GameEngine::Engine::Renderer::Pipeline::Nodes::VirtualShadowRequestReuse;
    using Phase = std::array<VirtualShadowMarkProjectionGPU, kProjections>;
    std::array<Phase, 3> phases{projections, projections, projections};
    const std::array<float, 3> phaseDepth{0.5f, 0.25f, 0.75f};
    for (uint32_t phase = 0; phase < phases.size(); ++phase)
        for (auto& projection : phases[phase])
            for (uint32_t axis = 0; axis < 4; ++axis)
                projection.ViewClipToLightClip.Data()[12 + axis] +=
                    (static_cast<float>(phase) * 0.5f / kWidth) * projection.ViewClipToLightClip.Data()[axis];
    const auto runMarker = [&](uint32_t phase, bool clear, BufferHandle target)
    {
        depth[(kViewportY + 256) * kTextureExtent + kViewportX + 255] = phaseDepth[phase];
        ASSERT_NO_FATAL_FAILURE(Upload(upload, depth.data(), depthBytes));
        ASSERT_NO_FATAL_FAILURE(Upload(projectionBuffer, phases[phase].data(), sizeof(projections)));
        m_Device->UpdateStorageBufferBinding(m_Set, 2, target, 0, sizeof(Words));
        auto mark = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_NE(mark, nullptr);
        mark->Begin();
        mark->Barrier(ResourceBarrier::CreateTextureBarrier(m_Depth, ResourceState::ShaderResource, ResourceState::CopyDest));
        mark->CopyBufferToTextureSubresource(upload, m_Depth, 0, 0, kTextureExtent, kTextureExtent);
        mark->Barrier(ResourceBarrier::CreateTextureBarrier(m_Depth, ResourceState::CopyDest, ResourceState::ShaderResource));
        if (clear)
        {
            mark->Barrier(ResourceBarrier::CreateBufferBarrier(target, ResourceState::UnorderedAccess, ResourceState::CopyDest));
            mark->FillBuffer(target, 0, sizeof(Words), 0);
            mark->Barrier(ResourceBarrier::CreateBufferBarrier(target, ResourceState::CopyDest, ResourceState::UnorderedAccess));
        }
        else
            mark->Barrier(ResourceBarrier::CreateBufferBarrier(target, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess));
        mark->SetPipeline(pipeline);
        mark->BindDescriptorSet(0, m_Set, pipeline);
        mark->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, kProjections);
        mark->End();
        m_Device->ExecuteCommandLists({mark.get()});
        m_Device->WaitForIdle();
        ASSERT_EQ(m_Device->GetDeviceHealth(), DeviceHealth::Healthy);
    };
    const auto readWords = [&](BufferHandle target)
    {
        Words value{};
        void* data = m_Device->MapBuffer(target);
        EXPECT_NE(data, nullptr);
        if (data)
        {
            std::memcpy(value.data(), data, sizeof(value));
            m_Device->UnmapBuffer(target);
        }
        return value;
    };
    std::array<Words, 3> fresh{};
    // One reference buffer is sufficient: each dispatch completes before its
    // descriptor or upload memory changes, and every reference clears it.
    const auto referenceBuffer = MakeBuffer(sizeof(Words), BufferUsage::Storage | BufferUsage::TransferDst, BufferMemoryUsage::Readback);
    ASSERT_TRUE(referenceBuffer.IsValid());
    for (uint32_t phase = 0; phase < phases.size(); ++phase)
    {
        ASSERT_NO_FATAL_FAILURE(runMarker(phase, true, referenceBuffer));
        fresh[phase] = readWords(referenceBuffer);
    }
    for (uint32_t phase = 0; phase < fresh.size(); ++phase)
    {
        bool hasExclusiveBit = false;
        for (uint32_t word = 0; word < kWords; ++word)
            hasExclusiveBit |= (fresh[phase][word] & ~fresh[(phase + 1) % fresh.size()][word]) != 0;
        EXPECT_TRUE(hasExclusiveBit) << "phase witness must change actual requested pages: " << phase;
    }
    VirtualShadowRequestReuse cache;
    ElisionInputBlob domain;
    domain.Append(uint64_t{1});
    constexpr std::array<uint32_t, 8> sequence{0, 1, 0, 1, 0, 1, 2, 0};
    uint32_t dispatches = 0;
    for (uint64_t frame = 0; frame < sequence.size(); ++frame)
    {
        const uint32_t phase = sequence[frame];
        ElisionInputBlob key;
        key.AppendBytes(phases[phase].data(), sizeof(projections));
        key.Append(phaseDepth[phase]);
        key.Append(params);
        const bool reuse = cache.CanReuse(frame, domain, key, requests.id, false, true);
        EXPECT_EQ(reuse, frame == 5 || frame == 7);
        if (!reuse)
        {
            const auto write = cache.PrepareWrite();
            ASSERT_NO_FATAL_FAILURE(runMarker(phase, write.Clear, requests));
            ASSERT_TRUE(cache.MarkSubmitted(write.Ticket, key, requests.id));
            ++dispatches;
        }
        const auto retained = readWords(requests);
        for (uint32_t word = 0; word < kWords; ++word)
            EXPECT_EQ(retained[word] & fresh[phase][word], fresh[phase][word]);
    }
    Words unionExpected{};
    for (uint32_t word = 0; word < kWords; ++word)
        unionExpected[word] = fresh[0][word] | fresh[1][word] | fresh[2][word];
    EXPECT_EQ(readWords(requests), unionExpected);
    EXPECT_EQ(dispatches, 6u);
    RecordProperty("phase_union_dispatches", dispatches);

}

} // namespace
