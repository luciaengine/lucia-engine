#include "Engine/Rendering/2D/SpriteRenderFeature.h"

#include "EngineTestShaderSetup.h"
#include "Mathematics/Vector4.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Types/Color.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
class SpriteAtlasSampling : public testing::Test
{
protected:
    static void ExpectBlue(const std::vector<ColorLinear>& pixels)
    {
        ASSERT_FALSE(pixels.empty());
        float32 maximumRed = 0;
        float32 minimumBlue = 1;
        for (const auto& pixel : pixels)
        {
            maximumRed = std::max(maximumRed, pixel.r);
            minimumBlue = std::min(minimumBlue, pixel.b);
        }
        EXPECT_NEAR(maximumRed, 0, 0.001f);
        EXPECT_NEAR(minimumBlue, 1, 0.001f);
    }

    void SetUp() override
    {
        m_Workspace = std::filesystem::temp_directory_path() / ("ge-sprite-atlas-" + GUID::Generate().ToString());
        DeviceDesc deviceDesc;
        deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
        deviceDesc.enableDebugLayer = false;
        deviceDesc.enableDynamicRendering = true;
        deviceDesc.pipelineCacheDirectory = (m_Workspace / "PipelineCache").string();
        m_Device = DeviceFactory::CreateDevice(deviceDesc);
        ASSERT_TRUE(m_Device && m_Device->Initialize(deviceDesc));
        Utils::SetShaderFileLoader(nullptr);
        Utils::SetShaderPathResolver(&Testing::EngineTestShaderPathResolver);
        ShaderPackage package;
        std::string error;
        ASSERT_TRUE(LoadShaderPkg("Shaders/sprite2d.shaderpkg", m_Device->PreferredShaderSource(), package, &error)) << error;
        m_Layout.bindings = {
            {0, DescriptorType::StorageBuffer, 1, kShaderStageVertex},
            {1, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
            {2, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
            {3, DescriptorType::StorageBuffer, 1, kShaderStageFragment},
            {5, DescriptorType::UniformBuffer, 1, kShaderStageVertex}};
        PipelineDesc pipeline;
        pipeline.type = PipelineType::Graphics;
        pipeline.vertexShader = package.stageBytes.at("vs");
        pipeline.pixelShader = package.stageBytes.at("fs");
        pipeline.topology = PrimitiveTopology::TriangleList;
        pipeline.EnableDepthTest(false);
        pipeline.SetCullingMode(CullModeFlagBits::None);
        pipeline.AddDynamicState(DynamicState::Viewport);
        pipeline.AddDynamicState(DynamicState::Scissor);
        pipeline.descriptorSetLayouts = {m_Layout};
        pipeline.pushConstantSize = sizeof(Engine::Renderer::SpritePushConstants);
        pipeline.pushConstantStagesMask = kShaderStageVertex;
        pipeline.colorAttachmentFormats = {uint32(TextureFormat::R32G32B32A32_FLOAT)};
        m_Pipeline = m_Device->CreatePipeline(pipeline);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        for (auto texture : m_Textures)
            m_Device->DestroyTexture(texture);
        if (m_Set.IsValid()) m_Device->DestroyDescriptorSet(m_Set);
        if (m_Sampler.IsValid()) m_Device->DestroySampler(m_Sampler);
        if (m_Pipeline.IsValid()) m_Device->DestroyPipeline(m_Pipeline);
        m_Device->Shutdown();
        std::error_code error;
        std::filesystem::remove_all(m_Workspace, error);
    }

    std::vector<ColorLinear> Render(bool linear, bool flipped, bool normalMapped, bool rotated)
    {
        m_Sampler = m_Device->CreateSampler(linear ? SamplerDesc::MaterialLinearClamp() : SamplerDesc::PointClamp());
        m_Set = m_Device->CreateDescriptorSet({m_Layout, "SpriteAtlasSampling", true});
        const std::array<uint8, 8> colors = {0, 0, 255, 255, 255, 0, 0, 255};
        const std::array<uint8, 16> normals = {128, 128, 255, 255, 128, 128, 255, 255,
                                              255, 128, 255, 255, 255, 128, 255, 255};
        const auto atlas = Upload(colors.data(), 2);
        const auto normal = Upload(normals.data(), 4);
        m_Device->UpdateCombinedImageSamplerBinding(m_Set, 1, atlas, m_Sampler);
        m_Device->UpdateCombinedImageSamplerBinding(m_Set, 2, normal, m_Sampler);

        // Mirrors the production instance buffer and sprite2d.vert std430 layout.
        struct Instance
        {
            Mathematics::Vector4 Center{0, 0, 0.5f, 0};
            Mathematics::Vector4 AxisX{1, 0, 0, 0};
            Mathematics::Vector4 AxisY{0, 1, 0, 0};
            Mathematics::Vector4 UVRect{0, 0, 0.5f, 1};
            ColorLinear Tint{1, 1, 1, 1};
            Mathematics::Vector4 Surface{0, 1, 0, 1};
        } instance;
        static_assert(sizeof(Instance) == 96);
        if (flipped) instance.UVRect = {0.5f, 1, 0, 0};
        if (normalMapped) instance.Surface = {1, 1, 1, 1};
        if (rotated)
        {
            instance.AxisX = {0, 1, 0, 0};
            instance.AxisY = {-1, 0, 0, 0};
        }
        BindBuffer(0, &instance, sizeof(instance), BufferUsage::Storage);
        CameraData camera{};
        for (int32 diagonal = 0; diagonal < 16; diagonal += 5)
            camera.view[diagonal] = camera.proj[diagonal] = camera.viewProj[diagonal] = 1;
        BindBuffer(5, &camera, sizeof(camera), BufferUsage::Uniform);
        Engine::Renderer::SpriteLight light{};
        light.DirectionFalloff[2] = 1;
        constexpr float32 kPi = 3.14159265359f;
        light.ColorRange[0] = light.ColorRange[1] = light.ColorRange[2] = kPi;
        BindBuffer(3, &light, sizeof(light), BufferUsage::Storage);

        constexpr uint32 kWidth = 64;
        constexpr uint32 kHeight = 32;
        TextureDesc targetDesc;
        targetDesc.width = kWidth;
        targetDesc.height = kHeight;
        targetDesc.format = uint32(TextureFormat::R32G32B32A32_FLOAT);
        targetDesc.usage = uint32(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        const auto target = m_Device->CreateTexture(targetDesc);
        m_Textures.push_back(target);
        const auto readback = m_Device->CreateReadbackBuffer(kWidth * kHeight * sizeof(ColorLinear));
        m_Buffers.push_back(readback);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc pass;
        pass.colorTargetCount = 1;
        pass.colorTargets[0] = target;
        pass.clearColor[0] = true;
        pass.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        commands->BeginRenderPass(pass);
        commands->SetViewport(0, 0, float32(kWidth), float32(kHeight));
        commands->SetScissor(0, 0, kWidth, kHeight);
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, m_Set, m_Pipeline);
        commands->SetPushConstants(Engine::Renderer::SpritePushConstants{});
        commands->Draw(6);
        commands->EndRenderPass();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::RenderTarget, ResourceState::CopySource));
        commands->CopyTextureToBuffer(target, readback, kWidth, kHeight);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        const auto* mapped = static_cast<const ColorLinear*>(m_Device->MapBuffer(readback));
        EXPECT_NE(mapped, nullptr);
        std::vector<ColorLinear> pixels;
        if (mapped) pixels.assign(mapped, mapped + kWidth * kHeight);
        m_Device->UnmapBuffer(readback);
        return pixels;
    }

private:
    TextureHandle Upload(const uint8* pixels, uint32 width)
    {
        TextureDesc desc;
        desc.width = width;
        desc.format = uint32(TextureFormat::RGBA8_UNORM);
        desc.usage = uint32(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        const auto texture = m_Device->CreateTexture(desc);
        m_Textures.push_back(texture);
        const auto staging = m_Device->CreateUploadBuffer(width * 4u);
        m_Buffers.push_back(staging);
        m_Device->UpdateBuffer(staging, 0, width * 4u, pixels);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->CopyBufferToTextureSubresource(staging, texture, 0, 0, width, 1);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        return texture;
    }

    void BindBuffer(uint32 binding, const void* data, size_t size, BufferUsage usage)
    {
        BufferDesc desc;
        desc.size = size;
        desc.usage = uint32(usage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        const auto buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        m_Device->UpdateBuffer(buffer, 0, size, data);
        if (usage == BufferUsage::Uniform)
            m_Device->UpdateBufferBinding(m_Set, binding, buffer, 0, size);
        else
            m_Device->UpdateStorageBufferBinding(m_Set, binding, buffer, 0, size);
    }

    std::unique_ptr<IDevice> m_Device;
    std::filesystem::path m_Workspace;
    PipelineHandle m_Pipeline;
    DescriptorSetLayoutDesc m_Layout;
    DescriptorSetHandle m_Set;
    SamplerHandle m_Sampler;
    std::vector<BufferHandle> m_Buffers;
    std::vector<TextureHandle> m_Textures;
};
}

TEST_F(SpriteAtlasSampling, LinearFilteringKeepsNeighboringFramesOutOfTheSprite)
{
    ExpectBlue(Render(true, false, false, false));
}

TEST_F(SpriteAtlasSampling, FlippedAndRotatedFramesKeepTheirSamplingBounds)
{
    ExpectBlue(Render(true, true, false, true));
}

TEST_F(SpriteAtlasSampling, NormalAtlasUsesItsOwnTexelSize)
{
    ExpectBlue(Render(true, false, true, false));
}

TEST_F(SpriteAtlasSampling, PointFilteringStillSamplesTheAuthoredFrame)
{
    ExpectBlue(Render(false, false, false, false));
}
