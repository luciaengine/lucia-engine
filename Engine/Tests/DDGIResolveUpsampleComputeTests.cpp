// Run the production DDGI irradiance upsample of the forward consume (Includes/ddgi_resolve_upsample.glsl, through the
// fixture ddgi_resolve_upsample_test.comp) on synthetic views: a crease between 2 perpendicular planes with no depth
// step, one plane at a 3/4 resolve scale, and pixels that no texel describes.
#include <gtest/gtest.h>

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/packing.hpp>

using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kRenderSize = 8;
constexpr float kFlatDepth = 0.5f;  // reverse-Z

// The render-extent depth of a view and the matrices that take a pixel to its world point.
struct UpsampleView
{
    std::vector<float> Depth;
    ViewParamsUBO Params{};
};

// Every pixel at one depth, identity matrices.
UpsampleView FlatView()
{
    UpsampleView view;
    view.Depth.assign(kRenderSize * kRenderSize, kFlatDepth);
    for (float* m : {view.Params.ge_invProj, view.Params.ge_invView})
        for (int i = 0; i < 4; ++i)
            m[i * 5] = 1.0f;
    return view;
}

// A narrow perspective view of 2 perpendicular planes in view space (+Z forward, +Y up) that meet on the horizontal
// line through the view centre at kCreaseDistance, as in DDGIResolveBlurComputeTests.cpp. Rows above the centre see
// a wall (z = d - y / kFloorSlope), rows below it a floor at a grazing angle (z = d + y * kFloorSlope).
constexpr float kCreaseDistance = 4.0f;
constexpr float kFloorSlope = 4.0f;
constexpr float kNearPlane = 0.1f;
constexpr float kTanHalfFov = 0.0043634f;  // 0.25 degrees: all depths of the view are within 1 % of each other

float CreaseDepth(float ndcY)
{
    const float slope = ndcY > 0.0f ? -1.0f / kFloorSlope : kFloorSlope;
    return kNearPlane * (1.0f - slope * ndcY * kTanHalfFov) / kCreaseDistance;
}

UpsampleView CreaseView()
{
    UpsampleView view;
    view.Depth.resize(kRenderSize * kRenderSize);
    for (uint32_t row = 0; row < kRenderSize; ++row)
    {
        const float ndcY = 1.0f - (2.0f * static_cast<float>(row) + 1.0f) / static_cast<float>(kRenderSize);
        for (uint32_t column = 0; column < kRenderSize; ++column)
            view.Depth[row * kRenderSize + column] = CreaseDepth(ndcY);
    }
    // Column-major inverse of a reverse-Z projection with depth = near / z: (ndc.xy, depth, 1) -> view point.
    view.Params.ge_invProj[0] = kTanHalfFov;
    view.Params.ge_invProj[5] = kTanHalfFov;
    view.Params.ge_invProj[11] = 1.0f / kNearPlane;
    view.Params.ge_invProj[14] = 1.0f;
    for (int i = 0; i < 4; ++i)
        view.Params.ge_invView[i * 5] = 1.0f;
    return view;
}

// The resolve-extent inputs of one run: irradiance (rgb, raw depth in a) and the gather normal of each texel.
struct ResolveTexels
{
    uint32_t Size = 0;
    std::vector<float> Irradiance;
    std::vector<float> Normal;

    explicit ResolveTexels(uint32_t size) : Size(size), Irradiance(size * size * 4), Normal(size * size * 4) {}

    void Set(uint32_t column, uint32_t row, float value, float depth, float nx, float ny, float nz)
    {
        float* irradiance = &Irradiance[(row * Size + column) * 4];
        irradiance[0] = irradiance[1] = irradiance[2] = value;
        irradiance[3] = depth;
        float* normal = &Normal[(row * Size + column) * 4];
        normal[0] = nx;
        normal[1] = ny;
        normal[2] = nz;
        normal[3] = 1.0f;
    }
};

class DDGIResolveUpsampleComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        ShaderPackage pkg{};
        std::string err;
        ASSERT_TRUE(LoadShaderPkg("Shaders/ddgi_resolve_upsample_test.shaderpkg",
                                  m_Device->PreferredShaderSource(), pkg, &err))
            << err;
        m_Meta = std::move(pkg.meta);
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(pkg.stageBytes.at("cs"));
        auto keepSet0 = [this](uint32_t set, DescriptorSetLayoutDesc& layout)
        {
            if (set == 0)
                m_Layout = layout;
        };
        MaterialHelper::ApplyShaderMetaToComputeDesc(*m_Device, m_Meta, cd, MaterialBuilder::MergeMode::Auto,
                                                     {true, 128}, keepSet0, &err);
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(std::move(cd)));
        ASSERT_TRUE(m_Pipeline.IsValid()) << err;
        // A linear sampler proves that the upsample fetches texels and never filters.
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("DDGIResolveUpsampleTest"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (TextureHandle texture : m_Textures)
            m_Device->DestroyTexture(texture);
        for (BufferHandle buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        m_Device->DestroySampler(m_Sampler);
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        m_Buffers.push_back(m_Device->CreateBuffer(desc));
        return m_Buffers.back();
    }

    // A size x size texture uploaded from `texels` (RGBA16F when four channels, else R32F), left in the
    // shader-read state.
    TextureHandle MakeTexture(CommandList& cl, uint32_t size, const std::vector<float>& texels, bool rgba)
    {
        TextureDesc desc{};
        desc.width = desc.height = size;
        desc.depth = desc.mipLevels = desc.arrayLayers = desc.sampleCount = 1;
        desc.format = static_cast<uint32_t>(rgba ? TextureFormat::R16G16B16A16_FLOAT : TextureFormat::R32_FLOAT);
        desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                                           TextureUsage::TransferSrc | TextureUsage::TransferDst);
        const TextureHandle texture = m_Device->CreateTexture(desc);
        m_Textures.push_back(texture);
        const size_t bytes = texels.size() * (rgba ? sizeof(uint16_t) : sizeof(float));
        const BufferHandle upload = MakeBuffer(bytes, BufferUsage::TransferSrc, BufferMemoryUsage::Upload);
        void* mapped = m_Device->MapBuffer(upload);
        if (rgba)
            for (size_t i = 0; i < texels.size(); ++i)
                static_cast<uint16_t*>(mapped)[i] = glm::packHalf1x16(texels[i]);
        else
            std::memcpy(mapped, texels.data(), bytes);
        m_Device->UnmapBuffer(upload);
        cl.Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined, ResourceState::CopyDest));
        cl.CopyBufferToTextureSubresource(upload, texture, 0, 0, size, size);
        cl.Barrier(
            ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        return texture;
    }

    // Upsamples `resolve` to the render extent of `view` and returns the red channel of each pixel.
    std::vector<float> RunUpsample(const ResolveTexels& resolve, const UpsampleView& view)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        const TextureHandle irradianceIn = MakeTexture(*cl, resolve.Size, resolve.Irradiance, true);
        const TextureHandle gatherNormal = MakeTexture(*cl, resolve.Size, resolve.Normal, true);
        const TextureHandle depth = MakeTexture(*cl, kRenderSize, view.Depth, false);
        const TextureHandle irradianceOut =
            MakeTexture(*cl, kRenderSize, std::vector<float>(kRenderSize * kRenderSize * 4, 0.0f), true);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(irradianceOut, ResourceState::ShaderResource,
                                                          ResourceState::UnorderedAccess));

        const BufferHandle viewParams =
            MakeBuffer(sizeof(view.Params), BufferUsage::Uniform, BufferMemoryUsage::Upload);
        std::memcpy(m_Device->MapBuffer(viewParams), &view.Params, sizeof(view.Params));
        m_Device->UnmapBuffer(viewParams);

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        dsDesc.transient = true;
        const DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);
        NamedDescriptorWriter writer(m_Device.get(), ds, m_Meta, 0);
        writer.AddCombinedImageSampler("uIrradianceIn", irradianceIn, m_Sampler);
        writer.AddCombinedImageSampler("uGatherNormal", gatherNormal, m_Sampler);
        writer.AddCombinedImageSampler("uViewDepth", depth, m_Sampler);
        writer.AddUniformBuffer("ViewParams", viewParams, 0, sizeof(view.Params));
        writer.Flush();
        GameEngine::Engine::Renderer::Pipeline::Nodes::Detail::BindStorageImageByName(
            m_Device.get(), ds, m_Meta, "uIrradianceOut", irradianceOut);

        cl->SetPipeline(m_Pipeline);
        cl->BindDescriptorSet(0, ds, m_Pipeline);
        cl->Dispatch(1, 1, 1);

        constexpr uint32_t kPixels = kRenderSize * kRenderSize;
        const BufferHandle readback = m_Device->CreateReadbackBuffer(kPixels * 4 * sizeof(uint16_t));
        m_Buffers.push_back(readback);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(irradianceOut, ResourceState::UnorderedAccess,
                                                          ResourceState::CopySource));
        cl->CopyTextureSubresourceToBuffer(irradianceOut, 0, 0, readback, kRenderSize, kRenderSize);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        std::vector<float> red(kPixels);
        const auto* halves = static_cast<const uint16_t*>(m_Device->MapBuffer(readback));
        for (uint32_t i = 0; i < kPixels; ++i)
            red[i] = glm::unpackHalf1x16(halves[i * 4]);
        m_Device->UnmapBuffer(readback);
        return red;
    }

    std::unique_ptr<IDevice> m_Device;
    ShaderMeta m_Meta;
    DescriptorSetLayoutDesc m_Layout;
    PipelineHandle m_Pipeline;
    SamplerHandle m_Sampler;
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

} // namespace

// Half resolve scale. The 2 resolve rows above the crease hold the light of the wall, the 2 below it the light of
// the floor, each with the normal of its plane. The depth agrees on all texels, so only the normal keeps the light
// of each plane on its own rows.
TEST_F(DDGIResolveUpsampleComputeTest, IrradianceDoesNotCrossACreaseWithNoDepthStep)
{
    constexpr float kWallLight = 1.0f;
    constexpr float kFloorLight = 0.0f;
    const float wallNormalLength = std::sqrt(1.0f + 1.0f / (kFloorSlope * kFloorSlope));
    const float floorNormalLength = std::sqrt(1.0f + kFloorSlope * kFloorSlope);
    ResolveTexels resolve(kRenderSize / 2);
    for (uint32_t row = 0; row < resolve.Size; ++row)
    {
        // The resolve reads the depth at the render pixel (2 * column + 1, 2 * row + 1).
        const float ndcY = 1.0f - (4.0f * static_cast<float>(row) + 3.0f) / static_cast<float>(kRenderSize);
        const bool wall = row < resolve.Size / 2;
        for (uint32_t column = 0; column < resolve.Size; ++column)
        {
            if (wall)
                resolve.Set(column, row, kWallLight, CreaseDepth(ndcY), 0.0f, -1.0f / (kFloorSlope * wallNormalLength),
                            -1.0f / wallNormalLength);
            else
                resolve.Set(column, row, kFloorLight, CreaseDepth(ndcY), 0.0f, kFloorSlope / floorNormalLength,
                            -1.0f / floorNormalLength);
        }
    }
    const std::vector<float> red = RunUpsample(resolve, CreaseView());
    for (uint32_t row = 0; row < kRenderSize; ++row)
        for (uint32_t column = 0; column < kRenderSize; ++column)
            EXPECT_NEAR(red[row * kRenderSize + column], row < kRenderSize / 2 ? kWallLight : kFloorLight, 1.0e-3f)
                << "pixel (" << column << ", " << row << ")";
}

// 3/4 resolve scale (6 texels to 8 pixels) on one plane: every texel agrees, so each pixel takes the bilinear
// interpolation of the 4 texels around its centre.
TEST_F(DDGIResolveUpsampleComputeTest, OnePlaneTakesTheBilinearInterpolationAtANonHalfScale)
{
    constexpr uint32_t kResolveSize = 6;
    constexpr float kLightPerColumn = 0.1f;
    ResolveTexels resolve(kResolveSize);
    for (uint32_t row = 0; row < kResolveSize; ++row)
        for (uint32_t column = 0; column < kResolveSize; ++column)
            resolve.Set(column, row, kLightPerColumn * static_cast<float>(column), kFlatDepth, 0.0f, 0.0f, -1.0f);
    const std::vector<float> red = RunUpsample(resolve, FlatView());
    for (uint32_t column = 0; column < kRenderSize; ++column)
    {
        const float footprint = (static_cast<float>(column) + 0.5f) * kResolveSize / kRenderSize - 0.5f;
        const float expected = kLightPerColumn * std::clamp(footprint, 0.0f, static_cast<float>(kResolveSize - 1));
        for (uint32_t row = 0; row < kRenderSize; ++row)
            EXPECT_NEAR(red[row * kRenderSize + column], expected, 2.0e-3f)
                << "pixel (" << column << ", " << row << ")";
    }
}

// No texel has the depth of the pixels (a sliver thinner than a resolve texel). Each pixel takes its nearest texel,
// not black.
TEST_F(DDGIResolveUpsampleComputeTest, PixelThatNoTexelDescribesTakesTheNearestTexel)
{
    constexpr uint32_t kResolveSize = kRenderSize / 2;
    constexpr float kOtherDepth = 0.9f;
    ResolveTexels resolve(kResolveSize);
    for (uint32_t row = 0; row < kResolveSize; ++row)
        for (uint32_t column = 0; column < kResolveSize; ++column)
            resolve.Set(column, row, 0.1f * static_cast<float>(1 + row * kResolveSize + column), kOtherDepth, 0.0f,
                        0.0f, -1.0f);
    const std::vector<float> red = RunUpsample(resolve, FlatView());
    for (uint32_t row = 0; row < kRenderSize; ++row)
        for (uint32_t column = 0; column < kRenderSize; ++column)
        {
            const float expected = 0.1f * static_cast<float>(1 + (row / 2) * kResolveSize + column / 2);
            EXPECT_NEAR(red[row * kRenderSize + column], expected, 2.0e-3f)
                << "pixel (" << column << ", " << row << ")";
        }
}
