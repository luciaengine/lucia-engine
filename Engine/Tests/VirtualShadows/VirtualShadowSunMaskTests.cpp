// The sun mask pass gives each prepass receiver the visibility that GE_TryVirtualShadowDirectional
// gives it in the world pass. Fixture: a sloped receiver, a rotated jittered camera, and a lit page,
// an occluder page and a missing page. The world pass read (GE_ReadVirtualShadowSunMask) gives a
// fragment the texel of its own pixel or of the neighbour on its depth plane.

#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowLookup.h"

#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
namespace Math = GameEngine::Mathematics;

namespace
{
constexpr uint32_t kSide = 32;          // mask and prepass depth, pixels
constexpr uint32_t kSkyRows = 3;        // top rows without a receiver
constexpr uint32_t kPageSide = 128;     // atlas layer, texels
constexpr float kWindow = 4.0f;         // half extent of the light window, world units
constexpr float kSlope = 0.2f;          // the receiver plane y = kSlope * x
constexpr uint32_t kMissing = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kNoPage = 255;       // kGE_VirtualShadowSunMaskNoPage
constexpr float kSteps = 254.0f;        // kGE_VirtualShadowSunMaskSteps

// Mirrors VirtualShadowSunMaskParams in virtual_shadow_sun_mask.comp.
struct MaskParams
{
    Math::Matrix4x4 ClipToPosition;
    Math::Vector4 ViewDepth;
    Math::Vector4 CameraPosition;
    Math::Vector4 TowardLight;
    int32_t Viewport[4]; // ivec4: origin xy, size zw in pixels
};

// Mirrors Probe and SunMaskTestParams in virtual_shadow_sun_mask_test.comp.
struct Probe
{
    Math::Vector4 Position;
    Math::Vector4 Normal;
    Math::Vector4 Screen;
};
struct ReferenceParams
{
    Math::Vector4 TowardLight;
    uint32_t Count[4]; // uvec4: x = probe count
};

static_assert(sizeof(MaskParams) == 128 && sizeof(Probe) == 48 && sizeof(ReferenceParams) == 32);

struct Fixture
{
    std::vector<float> Depth;
    std::vector<Probe> Probes;
    std::vector<uint32_t> ProbePixel;
    MaskParams Mask{};
};

glm::vec4 Multiply(const Math::Matrix4x4& matrix, const glm::vec4& vector)
{
    return matrix[0] * vector.x + matrix[1] * vector.y + matrix[2] * vector.z + matrix[3] * vector.w;
}

Fixture BuildFixture()
{
    const Math::Vector3 eye(0.5f, 5.0f, -7.0f);
    const Math::Matrix4x4 view = Math::MakeLookAtLH(eye, Math::Vector3(0.5f, 0.0f, 0.5f), Math::Vector3(0, 1, 0));
    Math::Matrix4x4 projection = Math::MakePerspectiveLH_ZO_ReverseZ(1.0f, 1.0f, 0.1f, 100.0f);
    // A TAA jitter of a fraction of a pixel in NDC.
    projection[2][0] += 0.37f * 2.0f / kSide;
    projection[2][1] -= 0.21f * 2.0f / kSide;
    const Math::Matrix4x4 viewProjection = projection * view;
    const Math::Matrix4x4 clipToPosition = Math::Inverse(viewProjection);

    Fixture fixture;
    fixture.Mask.ClipToPosition = clipToPosition;
    fixture.Mask.ViewDepth = Math::Vector4(view[0][2], view[1][2], view[2][2], view[3][2]);
    fixture.Mask.CameraPosition = Math::Vector4(eye.x, eye.y, eye.z, 1.0f);
    fixture.Mask.TowardLight = Math::Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    fixture.Mask.Viewport[2] = fixture.Mask.Viewport[3] = static_cast<int32_t>(kSide);

    const glm::vec3 normal = glm::normalize(glm::vec3(-kSlope, 1.0f, 0.0f));
    fixture.Depth.assign(kSide * kSide, 0.0f);
    for (uint32_t y = kSkyRows; y < kSide; ++y)
        for (uint32_t x = 0; x < kSide; ++x)
        {
            const float ndcX = (static_cast<float>(x) + 0.5f) / kSide * 2.0f - 1.0f;
            const float ndcY = (1.0f - (static_cast<float>(y) + 0.5f) / kSide) * 2.0f - 1.0f;
            glm::vec4 nearPoint = Multiply(clipToPosition, glm::vec4(ndcX, ndcY, 1.0f, 1.0f));
            glm::vec4 farPoint = Multiply(clipToPosition, glm::vec4(ndcX, ndcY, 0.0f, 1.0f));
            const glm::vec3 a = glm::vec3(nearPoint) / nearPoint.w;
            const glm::vec3 b = glm::vec3(farPoint) / farPoint.w;
            const float fa = a.y - kSlope * a.x;
            const float fb = b.y - kSlope * b.x;
            if (fa * fb > 0.0f)
                continue;
            const glm::vec3 hit = a + (b - a) * (fa / (fa - fb));
            const glm::vec4 clip = Multiply(viewProjection, glm::vec4(hit, 1.0f));
            const float depth = clip.z / clip.w;
            fixture.Depth[y * kSide + x] = depth;
            // The position the mask pass reconstructs from this depth.
            glm::vec4 position = Multiply(clipToPosition, glm::vec4(ndcX, ndcY, depth, 1.0f));
            position /= position.w;
            fixture.Probes.push_back({Math::Vector4(position.x, position.y, position.z, 1.0f),
                                      Math::Vector4(normal.x, normal.y, normal.z, 0.0f),
                                      Math::Vector4(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, 0, 0)});
            fixture.ProbePixel.push_back(y * kSide + x);
        }
    return fixture;
}

class VirtualShadowSunMaskTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (const auto set : m_Sets)
            m_Device->DestroyDescriptorSet(set);
        for (const auto texture : m_Textures)
            m_Device->DestroyTexture(texture);
        for (const auto sampler : m_Samplers)
            m_Device->DestroySampler(sampler);
        for (const auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
    }

    BufferHandle CreateBuffer(size_t bytes, uint32_t usage, BufferMemoryUsage memory, const void* source = nullptr)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = usage;
        desc.memoryUsage = memory;
        desc.debugName = "VirtualShadowSunMaskTest.Buffer";
        const auto handle = m_Device->CreateBuffer(desc);
        if (!handle.IsValid())
            return handle;
        m_Buffers.push_back(handle);
        if (source)
        {
            std::memcpy(m_Device->MapBuffer(handle), source, bytes);
            m_Device->UnmapBuffer(handle);
        }
        return handle;
    }

    TextureHandle CreateTexture(const TextureDesc& desc)
    {
        const auto handle = m_Device->CreateTexture(desc);
        if (handle.IsValid())
            m_Textures.push_back(handle);
        return handle;
    }

    PipelineHandle CreatePipeline(const char* package, const DescriptorSetLayoutDesc& layout)
    {
        ShaderPackage shader{};
        std::string error;
        if (!LoadShaderPkg(package, m_Device->PreferredShaderSource(), shader, &error))
        {
            ADD_FAILURE() << error;
            return {};
        }
        auto stage = shader.stageBytes.find("cs");
        if (stage == shader.stageBytes.end())
            return {};
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(stage->second));
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        desc.DebugName = package;
        return m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
    }

    DescriptorSetHandle CreateSet(const DescriptorSetLayoutDesc& layout)
    {
        DescriptorSetDesc desc{};
        desc.layout = layout;
        const auto set = m_Device->CreateDescriptorSet(desc);
        if (set.IsValid())
            m_Sets.push_back(set);
        return set;
    }

    // Runs the mask pass and the in-shader lookup over the fixture with one filter. quality is the
    // ShadowFilterQuality value in ShadowData; tanHalfAngle is the sun's, which sizes the PCSS penumbra.
    void CompareMaskWithLookup(float quality, float tanHalfAngle);

    // Runs GE_ReadVirtualShadowSunMask over fragments against a mask uploaded from the CPU.
    // Each result is (1 when resolved else 0, visibility).
    std::vector<glm::vec2> ReadMask(const std::vector<uint32_t>& mask, uint32_t side,
                                    const std::vector<glm::vec4>& fragments);

    std::unique_ptr<IDevice> m_Device;
    std::vector<DescriptorSetHandle> m_Sets;
    std::vector<TextureHandle> m_Textures;
    std::vector<SamplerHandle> m_Samplers;
    std::vector<BufferHandle> m_Buffers;
};

DescriptorSetLayoutDesc Layout(std::initializer_list<std::pair<uint32_t, DescriptorType>> entries)
{
    DescriptorSetLayoutDesc layout{};
    for (const auto& [index, type] : entries)
    {
        DescriptorBinding binding{};
        binding.binding = index;
        binding.type = type;
        binding.count = 1;
        binding.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(binding);
    }
    return layout;
}
} // namespace

void VirtualShadowSunMaskTest::CompareMaskWithLookup(float quality, float tanHalfAngle)
{
    m_Device = CreateVulkanDeviceFast();
    if (!m_Device)
        GTEST_SKIP() << "No graphics device available";
    RecordProperty("graphics_api", static_cast<int>(m_Device->GetAPI()));
    if (!m_Device->IsTextureFormatSupported(TextureFormat::R32_UINT,
            static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource)))
        GTEST_SKIP() << "No R32_UINT storage images on this device";

    const Fixture fixture = BuildFixture();
    ASSERT_FALSE(fixture.Probes.empty());

    using Type = DescriptorType;
    const auto virtualBindings = {std::pair{8u, Type::UniformBuffer}, std::pair{58u, Type::CombinedImageSampler},
                                  std::pair{59u, Type::StorageBuffer}, std::pair{60u, Type::StorageBuffer},
                                  std::pair{61u, Type::CombinedImageSampler}};
    DescriptorSetLayoutDesc maskLayout = Layout({{0u, Type::CombinedImageSampler}, {1u, Type::StorageImage},
                                                 {2u, Type::UniformBuffer}});
    DescriptorSetLayoutDesc referenceLayout = Layout({{0u, Type::StorageBuffer}, {1u, Type::StorageBuffer},
                                                      {2u, Type::UniformBuffer}});
    for (const auto& [index, type] : virtualBindings)
    {
        maskLayout.bindings.push_back(Layout({{index, type}}).bindings.front());
        referenceLayout.bindings.push_back(Layout({{index, type}}).bindings.front());
    }
    const auto maskPipeline = CreatePipeline("Shaders/virtual_shadow_sun_mask.shaderpkg", maskLayout);
    const auto referencePipeline = CreatePipeline("Shaders/virtual_shadow_sun_mask_test.shaderpkg", referenceLayout);
    ASSERT_TRUE(maskPipeline.IsValid() && referencePipeline.IsValid());

    // Atlas layers: 0 unused, 1 lit everywhere (0.25 is farther from the light than every receiver),
    // 2 with an occluder (0.75) over its left half.
    TextureDesc atlasDesc{};
    atlasDesc.width = atlasDesc.height = kPageSide;
    atlasDesc.depth = atlasDesc.mipLevels = atlasDesc.sampleCount = 1;
    atlasDesc.arrayLayers = 3;
    atlasDesc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    atlasDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::DepthStencil);
    atlasDesc.debugName = "VirtualShadowSunMaskTest.Atlas";
    const auto atlas = CreateTexture(atlasDesc);
    TextureDesc depthDesc{};
    depthDesc.width = depthDesc.height = kSide;
    depthDesc.depth = depthDesc.mipLevels = depthDesc.sampleCount = depthDesc.arrayLayers = 1;
    depthDesc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    depthDesc.debugName = "VirtualShadowSunMaskTest.PrepassDepth";
    const auto depth = CreateTexture(depthDesc);
    TextureDesc maskDesc = depthDesc;
    maskDesc.format = static_cast<uint32_t>(TextureFormat::R32_UINT);
    maskDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                           TextureUsage::TransferSrc);
    maskDesc.debugName = "VirtualShadowSunMaskTest.Mask";
    const auto mask = CreateTexture(maskDesc);
    ASSERT_TRUE(atlas.IsValid() && depth.IsValid() && mask.IsValid());
    const auto pointSampler = m_Device->CreateSampler(SamplerDesc::PointClamp("VirtualShadowSunMaskTest"));
    const auto compareSampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("VirtualShadowSunMaskTest.Cmp"));
    m_Samplers = {pointSampler, compareSampler};
    ASSERT_TRUE(pointSampler.IsValid() && compareSampler.IsValid());

    // One directional level: light clip x = x / kWindow, y = z / kWindow, depth = 0.5 + 0.5 * y.
    struct Projections
    {
        VirtualShadowLookupHeaderGPU Header{};
        VirtualShadowLookupProjectionGPU Row{};
    } projections;
    projections.Header.DirectionalRows = {0, kMissing, kMissing, kMissing};
    projections.Row.RelativeToLightClip = Math::Matrix4x4(glm::mat4(0.0f));
    projections.Row.RelativeToLightClip.Data()[0] = 1.0f / kWindow;
    projections.Row.RelativeToLightClip.Data()[9] = 1.0f / kWindow;
    projections.Row.RelativeToLightClip.Data()[6] = 0.5f;
    projections.Row.RelativeToLightClip.Data()[14] = 0.5f;
    projections.Row.RelativeToLightClip.Data()[15] = 1.0f;
    projections.Row.FirstPage = 0;
    projections.Row.PagesPerAxis = 2;
    projections.Row.LastMip = 1;
    const float worldPerTexel = 2.0f * kWindow / (2.0f * kPageSide);
    projections.Row.LightPositionRelAndFar[0] = worldPerTexel;
    projections.Row.LightPositionRelAndFar[1] = 2.0f;
    // Fine pages 0 to 3 (the page at +x, -z is missing), then the coarse page.
    constexpr std::array table{1u, 2u, 1u, kMissing, 1u};

    ShadowDataGPU shadowData{};
    shadowData.shadowParams[1] = 1.0f;   // normal bias
    shadowData.shadowParams[2] = 1.0f;   // one level
    shadowData.shadowParams[3] = 100.0f; // max shadow distance
    shadowData.shadowDebug[3] = quality;
    shadowData.shadowPcss[0] = 2.0f;     // softness
    shadowData.shadowPcss[1] = 16.0f;    // taps
    shadowData.shadowPcssCascades[0][0] = worldPerTexel;
    shadowData.shadowPcssCascades[0][1] = 2.0f;
    shadowData.shadowPcssCascades[0][3] = tanHalfAngle;

    const uint32_t storage = static_cast<uint32_t>(BufferUsage::Storage);
    const uint32_t uniform = static_cast<uint32_t>(BufferUsage::Uniform);
    const auto projectionBuffer = CreateBuffer(sizeof(projections), storage, BufferMemoryUsage::Upload, &projections);
    const auto tableBuffer = CreateBuffer(sizeof(table), storage, BufferMemoryUsage::Upload, table.data());
    const auto shadowBuffer = CreateBuffer(sizeof(shadowData), uniform, BufferMemoryUsage::Upload, &shadowData);
    const auto maskParams = CreateBuffer(sizeof(MaskParams), uniform, BufferMemoryUsage::Upload, &fixture.Mask);
    ReferenceParams referenceParams{};
    referenceParams.TowardLight = Math::Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    referenceParams.Count[0] = static_cast<uint32_t>(fixture.Probes.size());
    const auto referenceParamBuffer =
        CreateBuffer(sizeof(referenceParams), uniform, BufferMemoryUsage::Upload, &referenceParams);
    const size_t probeBytes = fixture.Probes.size() * sizeof(Probe);
    const size_t resultBytes = fixture.Probes.size() * sizeof(float);
    const auto probeBuffer = CreateBuffer(probeBytes, storage, BufferMemoryUsage::Upload, fixture.Probes.data());
    const auto resultBuffer = CreateBuffer(resultBytes, storage, BufferMemoryUsage::Readback);
    const auto depthUpload = CreateBuffer(fixture.Depth.size() * sizeof(float),
        static_cast<uint32_t>(BufferUsage::TransferSrc), BufferMemoryUsage::Upload, fixture.Depth.data());
    const size_t maskBytes = kSide * kSide * sizeof(uint32_t);
    const auto maskReadback = CreateBuffer(maskBytes, static_cast<uint32_t>(BufferUsage::TransferDst),
                                           BufferMemoryUsage::Readback);
    ASSERT_TRUE(projectionBuffer.IsValid() && tableBuffer.IsValid() && shadowBuffer.IsValid() &&
                maskParams.IsValid() && referenceParamBuffer.IsValid() && probeBuffer.IsValid() &&
                resultBuffer.IsValid() && depthUpload.IsValid() && maskReadback.IsValid());

    const auto maskSet = CreateSet(maskLayout);
    const auto referenceSet = CreateSet(referenceLayout);
    ASSERT_TRUE(maskSet.IsValid() && referenceSet.IsValid());
    for (const auto set : {maskSet, referenceSet})
    {
        m_Device->UpdateBufferBinding(set, 8, shadowBuffer, 0, sizeof(shadowData));
        m_Device->UpdateCombinedImageSamplerBinding(set, 58, atlas, pointSampler);
        m_Device->UpdateStorageBufferBinding(set, 59, tableBuffer, 0, sizeof(table));
        m_Device->UpdateStorageBufferBinding(set, 60, projectionBuffer, 0, sizeof(projections));
        m_Device->UpdateCombinedImageSamplerBinding(set, 61, atlas, compareSampler);
    }
    m_Device->UpdateCombinedImageSamplerBinding(maskSet, 0, depth, pointSampler);
    m_Device->UpdateStorageImageBinding(maskSet, 1, mask);
    m_Device->UpdateBufferBinding(maskSet, 2, maskParams, 0, sizeof(MaskParams));
    m_Device->UpdateStorageBufferBinding(referenceSet, 0, probeBuffer, 0, probeBytes);
    m_Device->UpdateStorageBufferBinding(referenceSet, 1, resultBuffer, 0, resultBytes);
    m_Device->UpdateBufferBinding(referenceSet, 2, referenceParamBuffer, 0, sizeof(referenceParams));

    ShaderPackage depthPackage{};
    std::string error;
    ASSERT_TRUE(LoadShaderPkg("Shaders/virtual_shadow_test_depth.shaderpkg", m_Device->PreferredShaderSource(),
                              depthPackage, &error)) << error;
    const auto vertex = depthPackage.stageBytes.find("vs");
    ASSERT_NE(vertex, depthPackage.stageBytes.end());
    GraphicsPipelineDesc occluderDesc{};
    occluderDesc.VertexShader = std::make_shared<const std::vector<uint8_t>>(vertex->second);
    occluderDesc.Rasterization.cullMode = CullModeFlagBits::None;
    occluderDesc.DepthStencil.depthTestEnable = occluderDesc.DepthStencil.depthWriteEnable = true;
    occluderDesc.DepthStencil.depthCompareOp = CompareOp::Always;
    occluderDesc.DebugName = "VirtualShadowSunMaskTest.Occluder";
    PipelineFormatKey format{};
    format.DepthFormat = TextureFormat::D32_FLOAT;
    const auto occluderPipeline =
        m_Device->GetOrCreateGraphicsPipeline(m_Device->InternGraphicsPipeline(occluderDesc), format);
    ASSERT_TRUE(occluderPipeline.IsValid());

    auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commands, nullptr);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(atlas, ResourceState::Undefined,
                                                            ResourceState::DepthWrite, 0, 1, 0, 3));
    for (uint32_t layer = 0; layer < 3; ++layer)
    {
        RenderPassDesc pass{};
        pass.depthTarget = atlas;
        pass.clearDepthValue = 0.25f;
        pass.useDepthView = true;
        pass.depthViewDesc.aspect = TextureAspect::Depth;
        pass.depthViewDesc.baseLayer = layer;
        pass.depthViewDesc.layerCount = 1;
        pass.depthViewDesc.levelCount = 1;
        commands->BeginRenderPass(pass);
        if (layer == 2)
        {
            commands->SetViewport(0, 0, kPageSide, kPageSide);
            commands->SetPipeline(occluderPipeline);
            commands->SetScissor(0, 0, kPageSide / 2, kPageSide);
            commands->Draw(3);
        }
        commands->EndRenderPass();
    }
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(atlas, ResourceState::DepthWrite,
                                                            ResourceState::ShaderResource, 0, 1, 0, 3));
    commands->CopyBufferToTextureSubresource(depthUpload, depth, 0, 0, kSide, kSide);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(depth, ResourceState::CopyDest,
                                                            ResourceState::ShaderResource));
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(mask, ResourceState::Undefined,
                                                            ResourceState::UnorderedAccess));
    commands->SetPipeline(maskPipeline);
    commands->BindDescriptorSet(0, maskSet, maskPipeline);
    commands->Dispatch(kSide / 8, kSide / 8, 1);
    commands->SetPipeline(referencePipeline);
    commands->BindDescriptorSet(0, referenceSet, referencePipeline);
    commands->Dispatch(static_cast<uint32_t>((fixture.Probes.size() + 63) / 64), 1, 1);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(mask, ResourceState::UnorderedAccess,
                                                            ResourceState::CopySource));
    commands->CopyTextureToBuffer(mask, maskReadback, kSide, kSide);
    commands->End();
    m_Device->ExecuteCommandLists({commands.get()});
    m_Device->WaitForIdle();

    std::vector<uint32_t> packed(kSide * kSide);
    std::memcpy(packed.data(), m_Device->MapBuffer(maskReadback), maskBytes);
    m_Device->UnmapBuffer(maskReadback);
    std::vector<float> reference(fixture.Probes.size());
    std::memcpy(reference.data(), m_Device->MapBuffer(resultBuffer), resultBytes);
    m_Device->UnmapBuffer(resultBuffer);

    for (uint32_t pixel = 0; pixel < kSkyRows * kSide; ++pixel)
        EXPECT_EQ(packed[pixel], 0u) << "a pixel without a receiver has no owner, pixel " << pixel;

    // One mask step of quantization, and the float error of the depth-reconstructed normal.
    constexpr float kTolerance = 1.0f / kSteps + 1e-3f;
    uint32_t lit = 0, shadowed = 0, penumbra = 0, noPage = 0;
    for (size_t probe = 0; probe < fixture.Probes.size(); ++probe)
    {
        const uint32_t pixel = fixture.ProbePixel[probe];
        const uint32_t texel = packed[pixel];
        uint32_t depthBits = 0;
        std::memcpy(&depthBits, &fixture.Depth[pixel], sizeof(depthBits));
        EXPECT_EQ(texel & 0xffffff00u, depthBits & 0xffffff00u) << "receiver depth, pixel " << pixel;
        const uint32_t value = texel & 255u;
        if (reference[probe] < 0.0f)
        {
            ++noPage;
            EXPECT_EQ(value, kNoPage) << "pixel " << pixel;
            continue;
        }
        ASSERT_NE(value, kNoPage) << "pixel " << pixel << " reference " << reference[probe];
        EXPECT_NEAR(static_cast<float>(value) / kSteps, reference[probe], kTolerance) << "pixel " << pixel;
        lit += reference[probe] > 0.95f;
        shadowed += reference[probe] < 0.05f;
        penumbra += reference[probe] >= 0.05f && reference[probe] <= 0.95f;
    }
    // The fixture covers every case: a lit page, the occluder, its penumbra and the missing page.
    EXPECT_GT(lit, 0u);
    EXPECT_GT(shadowed, 0u);
    EXPECT_GT(penumbra, 0u);
    EXPECT_GT(noPage, 0u);
}

TEST_F(VirtualShadowSunMaskTest, PoissonMaskEqualsTheInShaderLookupPerReceiver)
{
    CompareMaskWithLookup(2.0f, 0.0f);
}

// A wide light (tan 0.3) gives a penumbra of several texels over the occluder edge.
TEST_F(VirtualShadowSunMaskTest, PcssMaskEqualsTheInShaderLookupPerReceiver)
{
    CompareMaskWithLookup(3.0f, 0.3f);
}

namespace
{
// Mirrors kGE_VirtualShadowSunMaskSteps and GE_PackPrepassReceiver.
uint32_t PackReceiver(float depth, uint32_t value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &depth, sizeof(bits));
    return (bits & 0xffffff00u) | value;
}
} // namespace

std::vector<glm::vec2> VirtualShadowSunMaskTest::ReadMask(const std::vector<uint32_t>& mask, uint32_t side,
                                                          const std::vector<glm::vec4>& fragments)
{
    using Type = DescriptorType;
    const DescriptorSetLayoutDesc layout = Layout({{0u, Type::StorageBuffer}, {1u, Type::StorageBuffer},
                                                   {2u, Type::UniformBuffer}, {66u, Type::CombinedImageSampler}});
    const auto pipeline = CreatePipeline("Shaders/virtual_shadow_sun_mask_read_test.shaderpkg", layout);
    TextureDesc maskDesc{};
    maskDesc.width = maskDesc.height = side;
    maskDesc.depth = maskDesc.mipLevels = maskDesc.sampleCount = maskDesc.arrayLayers = 1;
    maskDesc.format = static_cast<uint32_t>(TextureFormat::R32_UINT);
    maskDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    maskDesc.debugName = "VirtualShadowSunMaskTest.ReadMask";
    const auto texture = CreateTexture(maskDesc);
    const auto sampler = m_Device->CreateSampler(SamplerDesc::PointClamp("VirtualShadowSunMaskTest.Read"));
    m_Samplers.push_back(sampler);
    const uint32_t storage = static_cast<uint32_t>(BufferUsage::Storage);
    const uint32_t count[4] = {static_cast<uint32_t>(fragments.size() / 2), 0, 0, 0};
    const size_t fragmentBytes = fragments.size() * sizeof(glm::vec4);
    const size_t resultBytes = count[0] * sizeof(glm::vec2);
    const auto fragmentBuffer = CreateBuffer(fragmentBytes, storage, BufferMemoryUsage::Upload, fragments.data());
    const auto resultBuffer = CreateBuffer(resultBytes, storage, BufferMemoryUsage::Readback);
    const auto params = CreateBuffer(sizeof(count), static_cast<uint32_t>(BufferUsage::Uniform),
                                     BufferMemoryUsage::Upload, count);
    const auto upload = CreateBuffer(mask.size() * sizeof(uint32_t), static_cast<uint32_t>(BufferUsage::TransferSrc),
                                     BufferMemoryUsage::Upload, mask.data());
    const auto set = CreateSet(layout);
    if (!pipeline.IsValid() || !texture.IsValid() || !sampler.IsValid() || !fragmentBuffer.IsValid() ||
        !resultBuffer.IsValid() || !params.IsValid() || !upload.IsValid() || !set.IsValid())
    {
        ADD_FAILURE() << "a GPU resource of the read test is not valid";
        return {};
    }
    m_Device->UpdateStorageBufferBinding(set, 0, fragmentBuffer, 0, fragmentBytes);
    m_Device->UpdateStorageBufferBinding(set, 1, resultBuffer, 0, resultBytes);
    m_Device->UpdateBufferBinding(set, 2, params, 0, sizeof(count));
    m_Device->UpdateCombinedImageSamplerBinding(set, 66, texture, sampler);

    auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->CopyBufferToTextureSubresource(upload, texture, 0, 0, side, side);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                            ResourceState::ShaderResource));
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, set, pipeline);
    commands->Dispatch((count[0] + 63) / 64, 1, 1);
    commands->End();
    m_Device->ExecuteCommandLists({commands.get()});
    m_Device->WaitForIdle();

    std::vector<glm::vec2> results(count[0]);
    std::memcpy(results.data(), m_Device->MapBuffer(resultBuffer), resultBytes);
    m_Device->UnmapBuffer(resultBuffer);
    return results;
}

// With MSAA the mask holds the receiver of sample 0. A fragment that owns only other samples of its
// pixel takes the neighbour texel on its depth plane, and the cascade where no texel matches.
TEST_F(VirtualShadowSunMaskTest, FragmentReadsTheNeighbourOnItsDepthPlane)
{
    m_Device = CreateVulkanDeviceFast();
    if (!m_Device)
        GTEST_SKIP() << "No graphics device available";
    RecordProperty("graphics_api", static_cast<int>(m_Device->GetAPI()));

    // Surface A: a plane with the depth gradient (0.01, 0.002) per pixel. Surface C: far behind it.
    constexpr uint32_t kReadSide = 8;
    const glm::vec2 gradient(0.01f, 0.002f);
    auto planeDepth = [&gradient](uint32_t x, uint32_t y) { return 0.5f + gradient.x * x + gradient.y * y; };
    auto planeValue = [](uint32_t x, uint32_t y) { return x + kReadSide * y + 1u; };
    std::vector<uint32_t> mask(kReadSide * kReadSide);
    for (uint32_t y = 0; y < kReadSide; ++y)
        for (uint32_t x = 0; x < kReadSide; ++x)
            mask[y * kReadSide + x] = PackReceiver(planeDepth(x, y), planeValue(x, y));
    auto setTexel = [&mask](uint32_t x, uint32_t y, float depth, uint32_t value)
    { mask[y * kReadSide + x] = PackReceiver(depth, value); };
    constexpr float kFar = 0.05f;
    // Pixel (3, 3): sample 0 is on surface B. Of its neighbours, only (2, 3) stays on A.
    for (uint32_t y = 2; y <= 4; ++y)
        for (uint32_t x = 2; x <= 4; ++x)
            setTexel(x, y, kFar, 251);
    setTexel(3, 3, 0.2f, 250);
    setTexel(2, 3, planeDepth(2, 3), planeValue(2, 3));
    // Pixel (5, 6): the only neighbour on A has no resident page.
    for (uint32_t y = 5; y <= 7; ++y)
        for (uint32_t x = 4; x <= 6; ++x)
            setTexel(x, y, kFar, 252);
    setTexel(6, 6, planeDepth(6, 6), kNoPage);
    // Pixel (6, 1): two neighbours on A's plane within the tolerance; (7, 1) is exact.
    for (uint32_t y = 0; y <= 2; ++y)
        for (uint32_t x = 5; x <= 7; ++x)
            setTexel(x, y, kFar, 253);
    setTexel(7, 1, planeDepth(7, 1), planeValue(7, 1));
    setTexel(5, 1, planeDepth(5, 1) + 0.004f, 249);

    auto fragment = [](uint32_t x, uint32_t y, float depth, glm::vec2 depthGradient)
    {
        return std::array{glm::vec4(x + 0.5f, y + 0.5f, depth, 0.0f),
                          glm::vec4(depthGradient.x, depthGradient.y, 0.0f, 0.0f)};
    };
    struct Case
    {
        const char* Name;
        std::array<glm::vec4, 2> Fragment;
        bool Resolved;
        uint32_t Value;
    };
    const Case cases[] = {
        {"receiver of its own pixel", fragment(1, 1, planeDepth(1, 1), gradient), true, planeValue(1, 1)},
        {"surface B owns sample 0", fragment(3, 3, 0.2f, glm::vec2(0.0f)), true, 250},
        {"surface A at an edge pixel", fragment(3, 3, planeDepth(3, 3), gradient), true, planeValue(2, 3)},
        {"no depth gradient, no plane", fragment(3, 3, planeDepth(3, 3), glm::vec2(0.0f)), false, 0},
        {"a surface no texel holds", fragment(3, 3, 0.9f, gradient), false, 0},
        {"the matched neighbour has no page", fragment(5, 6, planeDepth(5, 6), gradient), false, 0},
        {"the nearest of two matches", fragment(6, 1, planeDepth(6, 1), gradient), true, planeValue(7, 1)},
    };
    std::vector<glm::vec4> fragments;
    for (const Case& test : cases)
        fragments.insert(fragments.end(), test.Fragment.begin(), test.Fragment.end());
    const std::vector<glm::vec2> results = ReadMask(mask, kReadSide, fragments);
    ASSERT_EQ(results.size(), std::size(cases));
    for (size_t index = 0; index < std::size(cases); ++index)
    {
        const Case& test = cases[index];
        EXPECT_EQ(results[index].x > 0.5f, test.Resolved) << test.Name;
        if (test.Resolved)
            EXPECT_FLOAT_EQ(results[index].y, static_cast<float>(test.Value) / kSteps) << test.Name;
    }
}
