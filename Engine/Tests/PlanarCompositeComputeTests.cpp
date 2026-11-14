#include "Engine/Rendering/Pipeline/Nodes/PlanarReflectionNode.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
namespace
{
// Matches the production std140 block, including the new viewport at its end.
struct CompositeParams
{
    float ReflectionViewProj[16]{};
    float WorldToLocal[16]{};
    float Plane[4]{0, 0, 1, -.5f};
    float Settings[4]{1, 1, .01f, 0};
    float Flags[4]{};
    float Viewport[4]{.25f, .125f, .5f, .75f};
};

class PlanarCompositeComputeTest : public ::testing::Test
{
  protected:
    static constexpr uint32_t kSize = 8;
    using Pixel = std::array<float, 4>;
    void SetUp() override
    {
        Device = CreateVulkanDeviceFast();
        if (!Device) GTEST_SKIP() << "No Vulkan device available";
        ShaderPackage package;
        std::string error;
        ASSERT_TRUE(LoadShaderPkg("Shaders/planar_reflection_composite.shaderpkg",
                                  Device->PreferredShaderSource(), package, &error)) << error;
        ASSERT_TRUE(package.stageBytes.contains("cs"));
        for (uint32_t binding = 0; binding < 12; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = binding == 7 || binding == 10 ? DescriptorType::UniformBuffer
                : binding == 8 || binding == 9 ? DescriptorType::StorageImage
                : DescriptorType::CombinedImageSampler;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            Layout.bindings.push_back(b);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(package.stageBytes.at("cs"));
        desc.DescriptorSetLayouts.push_back(Device->InternDescriptorSetLayout(Layout));
        desc.DebugName = "Planar.CompositeViewportRegression";
        Pipeline = Device->GetOrCreateComputePipeline(Device->InternComputePipeline(desc));
        ASSERT_TRUE(Pipeline.IsValid());
        Sampler = Device->CreateSampler(SamplerDesc::MaterialLinearClamp("Planar.CompositeViewportRegression"));
        ASSERT_TRUE(Sampler.IsValid());
    }
    void TearDown() override
    {
        if (!Device) return;
        Device->WaitForIdle();
        for (auto set : Sets) Device->DestroyDescriptorSet(set);
        for (auto texture : Textures) Device->DestroyTexture(texture);
        for (auto buffer : Buffers) Device->DestroyBuffer(buffer);
        if (Sampler.IsValid()) Device->DestroySampler(Sampler);
    }
    BufferHandle MakeBuffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory,
                             const void* data = nullptr)
    {
        BufferDesc desc{};
        desc.size = bytes; desc.usage = static_cast<uint32_t>(usage); desc.memoryUsage = memory;
        const auto buffer = Device->CreateBuffer(desc);
        Buffers.push_back(buffer);
        if (data && buffer.IsValid())
        {
            void* mapped = Device->MapBuffer(buffer);
            if (!mapped) { ADD_FAILURE() << "Upload mapping failed"; return {}; }
            std::memcpy(mapped, data, bytes);
            Device->UnmapBuffer(buffer);
        }
        return buffer;
    }
    TextureHandle MakeTexture(TextureFormat format, bool output)
    {
        TextureDesc desc{};
        desc.width = desc.height = kSize;
        desc.depth = desc.arrayLayers = desc.mipLevels = desc.sampleCount = 1;
        desc.format = static_cast<uint32_t>(format);
        desc.usage = static_cast<uint32_t>(output
            ? TextureUsage::UnorderedAccess | TextureUsage::TransferSrc
            : TextureUsage::ShaderResource | TextureUsage::TransferDst);
        const auto texture = Device->CreateTexture(desc);
        Textures.push_back(texture);
        return texture;
    }
    void Run(bool letterboxed, float materialMask = 1.0f)
    {
        std::array<std::vector<Pixel>, 6> inputs;
        for (auto& input : inputs) input.resize(kSize * kSize);
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const uint32_t i = y * kSize + x;
                inputs[0][i] = {.25f, .25f, .25f, 1};
                inputs[1][i] = {(x + .5f) / kSize, (y + .5f) / kSize, 0, 1};
                inputs[2][i] = {.5f, 0, 0, 0}; // Positive depth even in the bars.
                inputs[3][i] = {.5f, .5f, 0, materialMask}; // Octahedral +Z, roughness, planar coverage.
                inputs[4][i] = {1, 1, 1, 0};
                inputs[5][i] = {0, 0, 0, 0};
            }
        ViewParamsUBO view{};
        CompositeParams params;
        for (uint32_t i = 0; i < 16; i += 5)
            view.ge_invProj[i] = view.ge_invView[i] = params.ReflectionViewProj[i] = 1;
        params.WorldToLocal[0] = .25f;
        params.WorldToLocal[6] = .25f; // Map world Y to the mirror's local Z.
        params.WorldToLocal[15] = 1;
        if (!letterboxed)
        {
            params.Viewport[0] = params.Viewport[1] = 0;
            params.Viewport[2] = params.Viewport[3] = 1;
        }
        const auto viewBuffer = MakeBuffer(sizeof(view), BufferUsage::Uniform, BufferMemoryUsage::Upload, &view);
        const auto paramsBuffer = MakeBuffer(sizeof(params), BufferUsage::Uniform, BufferMemoryUsage::Upload, &params);
        const auto readback = MakeBuffer(kSize * kSize * 8, BufferUsage::TransferDst, BufferMemoryUsage::Readback);
        ASSERT_TRUE(viewBuffer.IsValid() && paramsBuffer.IsValid() && readback.IsValid());
        DescriptorSetDesc setDesc{}; setDesc.layout = Layout;
        const auto set = Device->CreateDescriptorSet(setDesc); Sets.push_back(set);
        ASSERT_TRUE(set.IsValid());
        auto commands = Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        for (uint32_t binding = 0; binding < inputs.size(); ++binding)
        {
            const auto texture = MakeTexture(TextureFormat::R32G32B32A32_FLOAT, false);
            const auto upload = MakeBuffer(inputs[binding].size() * sizeof(Pixel), BufferUsage::TransferSrc,
                                            BufferMemoryUsage::Upload, inputs[binding].data());
            ASSERT_TRUE(texture.IsValid() && upload.IsValid());
            commands->CopyBufferToTextureSubresource(upload, texture, 0, 0, kSize, kSize, 0);
            commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                                     ResourceState::ShaderResource));
            Device->UpdateCombinedImageSamplerBinding(set, binding, texture, Sampler);
            if (binding == 1) Device->UpdateCombinedImageSamplerBinding(set, 11, texture, Sampler);
            if (binding == 5) Device->UpdateCombinedImageSamplerBinding(set, 6, texture, Sampler);
        }
        const auto output = MakeTexture(TextureFormat::R16G16B16A16_FLOAT, true);
        const auto mask = MakeTexture(TextureFormat::R16_FLOAT, true);
        ASSERT_TRUE(output.IsValid() && mask.IsValid());
        Device->UpdateBufferBinding(set, 7, viewBuffer, 0, sizeof(view));
        Device->UpdateBufferBinding(set, 10, paramsBuffer, 0, sizeof(params));
        Device->UpdateStorageImageBinding(set, 8, output);
        Device->UpdateStorageImageBinding(set, 9, mask);
        for (auto texture : {output, mask})
            commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined,
                                                                     ResourceState::UnorderedAccess));
        commands->SetPipeline(Pipeline);
        commands->BindDescriptorSet(0, set, Pipeline);
        commands->Dispatch(1, 1, 1);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(output, ResourceState::UnorderedAccess,
                                                                 ResourceState::CopySource));
        commands->CopyTextureToBuffer(output, readback, kSize, kSize);
        commands->End();
        Device->ExecuteCommandLists({commands.get()});
        Device->WaitForIdle();
        const auto* mapped = static_cast<const uint16_t*>(Device->MapBuffer(readback));
        ASSERT_NE(mapped, nullptr);
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const bool inside = materialMask > 0.5f && (!letterboxed || (x >= 2 && x < 6 && y >= 1 && y < 7));
                const float u = letterboxed ? (x + .5f - 2) / 4 : (x + .5f) / kSize;
                const float v = letterboxed ? (y + .5f - 1) / 6 : (y + .5f) / kSize;
                const auto* pixel = mapped + (y * kSize + x) * 4;
                EXPECT_NEAR(Mathematics::HalfToFloat(pixel[0]), .25f + (inside ? u : 0), .003f) << x << ',' << y;
                EXPECT_NEAR(Mathematics::HalfToFloat(pixel[1]), .25f + (inside ? 1 - v : 0), .003f) << x << ',' << y;
                EXPECT_NEAR(Mathematics::HalfToFloat(pixel[2]), .25f, .001f) << x << ',' << y;
            }
        Device->UnmapBuffer(readback);
    }
    std::unique_ptr<IDevice> Device;
    DescriptorSetLayoutDesc Layout;
    PipelineHandle Pipeline;
    SamplerHandle Sampler;
    std::vector<DescriptorSetHandle> Sets;
    std::vector<TextureHandle> Textures;
    std::vector<BufferHandle> Buffers;
};
TEST_F(PlanarCompositeComputeTest, ReconstructsInsideTheCameraViewportAndLeavesBarsUnchanged)
{
    Run(false); // Positive full-target control: catches a no-op compositor.
    Run(true);
}
TEST_F(PlanarCompositeComputeTest, ZeroMaterialCoverageLeavesTheSceneUnchanged)
{
    Run(false, 0.0f);
}

}
