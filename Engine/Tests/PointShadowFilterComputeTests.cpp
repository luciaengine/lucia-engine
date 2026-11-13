#include "Engine/Rendering/RenderServices.h"
#include "../Source/Engine/Rendering/RenderServicesDetail.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector4.h"
#include "Platform/Shell.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>

namespace
{
namespace Rendering = GameEngine::Rendering;
namespace Renderer = GameEngine::Engine::Renderer;
using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector4;

class PointShadowFilterComputeTest : public ::testing::Test
{
  protected:
    struct alignas(16) Query
    {
        Vector4 PositionSlot;
        Vector4 UVFaceDepth;
        Vector4 LightPositionMode;
    };
    static_assert(sizeof(Query) == 48);
    static constexpr uint32_t kAtlasResolution = 64;
    static constexpr uint32_t kWorkgroupSize = 64; // Matches point_shadow_filter_test.comp.
    static constexpr uint32_t kFaceCount = Renderer::kPointShadowFaceCount;
    static constexpr uint32_t kLayerCount = kFaceCount * kFaceCount;

    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No graphics device available";
        RecordProperty("graphics_backend", m_Device->GetAPI() == Rendering::GraphicsAPI::Metal ? "Metal" :
            m_Device->GetAPI() == Rendering::GraphicsAPI::Vulkan ? "Vulkan" : "Other");
        std::ifstream input(GameEngine::Platform::GetExecutablePath().parent_path() /
            "TestData/ShadowFilter/point_shadow_filter_test.spv", std::ios::binary);
        ASSERT_TRUE(input.good()) << "Point shadow regression shader was not built";
        Rendering::ComputePipelineDesc pipeline{};
        pipeline.ComputeShader = std::make_shared<const std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        for (uint32_t binding : {0u, 1u, 25u, 26u})
        {
            Rendering::DescriptorBinding descriptor{};
            descriptor.binding = binding;
            descriptor.type = binding == 26 ? Rendering::DescriptorType::CombinedImageSampler
                                            : Rendering::DescriptorType::StorageBuffer;
            descriptor.count = 1;
            descriptor.shaderStages = Rendering::kShaderStageCompute;
            m_Layout.bindings.push_back(descriptor);
        }
        pipeline.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        pipeline.DebugName = "Shadow.PointFilterRegression";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipeline));
        ASSERT_TRUE(m_Pipeline.IsValid());
        m_Sampler = m_Device->CreateSampler(
            Rendering::SamplerDesc::ShadowComparePCF("Shadow.PointFilterRegression"));
        ASSERT_TRUE(m_Sampler.IsValid());
        Renderer::PopulatePointShadowGeometry(m_Geometry, m_LightPosition, 20.0f, 0, 0, nullptr);
        ASSERT_TRUE(m_Geometry.valid);
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto descriptor : m_Descriptors)
            m_Device->DestroyDescriptorSet(descriptor);
        for (auto texture : m_Textures)
            m_Device->DestroyTexture(texture);
        for (auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
    }

    Vector3 Unproject(uint32_t face, float horizontal, float vertical, float depth) const
    {
        const auto inverse = GameEngine::Mathematics::Inverse(m_Geometry.lightVP[face]);
        const Vector4 position = inverse.Transform(
            Vector4{horizontal * 2.0f - 1.0f, 1.0f - vertical * 2.0f, depth, 1.0f});
        return {position.x / position.w, position.y / position.w, position.z / position.w};
    }

    Query MakeQuery(uint32_t slot, uint32_t face, float horizontal, float vertical) const
    {
        constexpr float kReferenceDepth = 0.4f;
        const Vector3 position = Unproject(face, horizontal, vertical, kReferenceDepth);
        return {{position.x, position.y, position.z, static_cast<float>(slot)},
                {horizontal, vertical, static_cast<float>(face), kReferenceDepth},
                {m_LightPosition.x, m_LightPosition.y, m_LightPosition.z, 0.0f}};
    }

    uint32_t SelectFace(const Vector3& direction) const
    {
        uint32_t selected = 0;
        for (uint32_t face = 1; face < kFaceCount; ++face)
            if (Vector3::Dot(direction, m_Geometry.direction[face]) >
                Vector3::Dot(direction, m_Geometry.direction[selected]))
                selected = face;
        return selected;
    }

    float ConstantFaceVisibility(const Vector3& position, uint32_t tileResolution, uint32_t litFace) const
    {
        const uint32_t face = SelectFace(position - m_LightPosition);
        const Vector4 clip = m_Geometry.lightVP[face].Transform(Vector4{position.x, position.y, position.z, 1});
        const float horizontal = (clip.x / clip.w * 0.5f + 0.5f) * tileResolution - 0.5f;
        const float vertical = (0.5f - clip.y / clip.w * 0.5f) * tileResolution - 0.5f;
        const float horizontalBase = std::floor(horizontal);
        const float verticalBase = std::floor(vertical);
        const float horizontalFraction = horizontal - horizontalBase;
        const float verticalFraction = vertical - verticalBase;
        float visibility = 0.0f;
        for (uint32_t cornerVertical = 0; cornerVertical < 2; ++cornerVertical)
            for (uint32_t cornerHorizontal = 0; cornerHorizontal < 2; ++cornerHorizontal)
            {
                const float cornerU = (horizontalBase + cornerHorizontal + 0.5f) / tileResolution;
                const float cornerV = (verticalBase + cornerVertical + 0.5f) / tileResolution;
                const bool outsideU = cornerU < 0 || cornerU > 1;
                const bool outsideV = cornerV < 0 || cornerV > 1;
                const float weight = (cornerHorizontal ? horizontalFraction : 1.0f - horizontalFraction) *
                    (cornerVertical ? verticalFraction : 1.0f - verticalFraction);
                float comparison = face == litFace ? 1.0f : 0.0f;
                if (outsideU || outsideV)
                {
                    const Vector3 cornerDirection = Unproject(face, cornerU, cornerV, clip.z / clip.w) -
                        m_LightPosition;
                    if (outsideU && outsideV)
                    {
                        const float alignment = Vector3::Dot(cornerDirection, m_Geometry.direction[litFace]);
                        comparison = alignment > 0 ? 1.0f / 3.0f : 0.0f;
                    }
                    else
                        comparison = SelectFace(cornerDirection) == litFace ? 1.0f : 0.0f;
                }
                visibility += weight * comparison;
            }
        return visibility;
    }

    std::vector<float> MakeDepths(uint32_t tileResolution) const
    {
        std::vector<float> depths(kAtlasResolution * kAtlasResolution * kLayerCount);
        for (uint32_t slot = 0; slot < kFaceCount; ++slot)
            for (uint32_t face = 0; face < kFaceCount; ++face)
                for (uint32_t vertical = 0; vertical < kAtlasResolution; ++vertical)
                    for (uint32_t horizontal = 0; horizontal < kAtlasResolution; ++horizontal)
                    {
                        const bool inside = horizontal < tileResolution && vertical < tileResolution;
                        const bool lit = (face == slot) == inside;
                        const uint32_t layer = slot * kFaceCount + face;
                        depths[(layer * kAtlasResolution + vertical) * kAtlasResolution + horizontal] =
                            lit ? 0.0f : 1.0f;
                    }
        return depths;
    }

    Rendering::BufferHandle CreateBuffer(uint64_t size, Rendering::BufferUsage usage,
                                          Rendering::BufferMemoryUsage memory, const void* data = nullptr)
    {
        Rendering::BufferDesc descriptor{};
        descriptor.size = size;
        descriptor.usage = static_cast<uint32_t>(usage);
        descriptor.memoryUsage = memory;
        descriptor.debugName = "Shadow.PointFilterRegression.Buffer";
        const auto buffer = m_Device->CreateBuffer(descriptor);
        m_Buffers.push_back(buffer);
        if (buffer.IsValid() && data)
        {
            void* mapped = m_Device->MapBuffer(buffer);
            if (mapped)
            {
                std::memcpy(mapped, data, size);
                m_Device->UnmapBuffer(buffer);
            }
            else
                ADD_FAILURE() << "Point filter upload buffer could not be mapped";
        }
        return buffer;
    }

    void Dispatch(uint32_t tileResolution, const std::vector<float>& depths,
                  const std::vector<Query>& queries, float depthBias = 0.0f)
    {
        ASSERT_EQ(depths.size(), kAtlasResolution * kAtlasResolution * kLayerCount);
        ASSERT_FALSE(queries.empty());
        std::array<Renderer::PointShadowSlotGPU, kFaceCount> slots{};
        for (uint32_t slot = 0; slot < kFaceCount; ++slot)
        {
            for (uint32_t face = 0; face < kFaceCount; ++face)
                std::memcpy(slots[slot].pointShadowVP[face], m_Geometry.lightVP[face].Data(),
                            sizeof(slots[slot].pointShadowVP[face]));
            slots[slot].pointShadowParams[0] = 1.0f;
            slots[slot].pointShadowParams[1] = static_cast<float>(slot * kFaceCount);
            slots[slot].pointShadowParams[2] = depthBias;
            slots[slot].pointShadowParams2[0] = static_cast<float>(tileResolution);
            slots[slot].pointShadowParams2[1] = m_Geometry.nearPlane;
            slots[slot].pointShadowParams2[2] = m_Geometry.farPlane;
            slots[slot].pointShadowParams2[3] = static_cast<float>(tileResolution) / kAtlasResolution;
        }
        Rendering::TextureDesc descriptor{};
        descriptor.width = descriptor.height = kAtlasResolution;
        descriptor.depth = descriptor.mipLevels = descriptor.sampleCount = 1;
        descriptor.arrayLayers = kLayerCount;
        descriptor.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
        descriptor.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource |
            Rendering::TextureUsage::TransferDst | Rendering::TextureUsage::DepthStencil);
        descriptor.debugName = "Shadow.PointFilterRegression.Depth";
        const auto texture = m_Device->CreateTexture(descriptor);
        m_Textures.push_back(texture);
        ASSERT_TRUE(texture.IsValid());
        const auto upload = CreateBuffer(depths.size() * sizeof(float), Rendering::BufferUsage::TransferSrc,
                                          Rendering::BufferMemoryUsage::Upload, depths.data());
        const auto parameters = CreateBuffer(queries.size() * sizeof(Query), Rendering::BufferUsage::Storage,
                                              Rendering::BufferMemoryUsage::Upload, queries.data());
        const auto slotBuffer = CreateBuffer(sizeof(slots), Rendering::BufferUsage::Storage,
                                              Rendering::BufferMemoryUsage::Upload, slots.data());
        const auto output = CreateBuffer(queries.size() * sizeof(float), Rendering::BufferUsage::Storage,
                                          Rendering::BufferMemoryUsage::Readback);
        for (auto buffer : {upload, parameters, slotBuffer, output})
            ASSERT_TRUE(buffer.IsValid());
        Rendering::DescriptorSetDesc setDescriptor{};
        setDescriptor.layout = m_Layout;
        const auto set = m_Device->CreateDescriptorSet(setDescriptor);
        m_Descriptors.push_back(set);
        ASSERT_TRUE(set.IsValid());
        m_Device->UpdateStorageBufferBinding(set, 0, parameters, 0, queries.size() * sizeof(Query));
        m_Device->UpdateStorageBufferBinding(set, 1, output, 0, queries.size() * sizeof(float));
        m_Device->UpdateStorageBufferBinding(set, 25, slotBuffer, 0, sizeof(slots));
        m_Device->UpdateCombinedImageSamplerBinding(set, 26, texture, m_Sampler);
        auto commands = m_Device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        commands->Begin();
        for (uint32_t layer = 0; layer < kLayerCount; ++layer)
            commands->CopyBufferToTextureSubresource(upload, texture, 0, layer,
                kAtlasResolution, kAtlasResolution, layer * kAtlasResolution * kAtlasResolution * sizeof(float));
        commands->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(texture,
            Rendering::ResourceState::CopyDest, Rendering::ResourceState::ShaderResource));
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        commands->Dispatch((static_cast<uint32_t>(queries.size()) + kWorkgroupSize - 1) / kWorkgroupSize, 1, 1);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        void* mapped = m_Device->MapBuffer(output);
        ASSERT_NE(mapped, nullptr);
        m_Results.resize(queries.size());
        std::memcpy(m_Results.data(), mapped, m_Results.size() * sizeof(float));
        m_Device->UnmapBuffer(output);
    }

    std::vector<float> m_Results;
    Renderer::PointShadowFrameInfo m_Geometry{};
    const Vector3 m_LightPosition{3.0f, -2.0f, 5.0f};

  private:
    std::unique_ptr<Rendering::IDevice> m_Device;
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::PipelineHandle m_Pipeline{};
    Rendering::SamplerHandle m_Sampler{};
    std::vector<Rendering::TextureHandle> m_Textures;
    std::vector<Rendering::BufferHandle> m_Buffers;
    std::vector<Rendering::DescriptorSetHandle> m_Descriptors;
};

TEST_F(PointShadowFilterComputeTest, BilinearFootprintsCrossEveryFaceEdgeWithinFullAndReducedTiles)
{
    for (uint32_t tileResolution : {kAtlasResolution, kAtlasResolution / 4})
    {
        std::vector<Query> queries;
        std::vector<float> expected;
        for (uint32_t face = 0; face < kFaceCount; ++face)
            for (uint32_t edge = 0; edge < 4; ++edge)
                for (float inset : {0.0f, 0.125f, 0.25f, 0.5f, 1.25f})
                {
                    const float coordinate = inset / tileResolution;
                    const float horizontal = edge < 2 ? (edge == 0 ? coordinate : 1.0f - coordinate) : 0.375f;
                    const float vertical = edge >= 2 ? (edge == 2 ? coordinate : 1.0f - coordinate) : 0.625f;
                    queries.push_back(MakeQuery(face, face, horizontal, vertical));
                    expected.push_back(std::min(0.5f + inset, 1.0f));
                }
        Dispatch(tileResolution, MakeDepths(tileResolution), queries);
        ASSERT_EQ(m_Results.size(), queries.size());
        for (size_t index = 0; index < queries.size(); ++index)
            EXPECT_NEAR(m_Results[index], expected[index], 0.004f)
                << "tile=" << tileResolution << " query=" << index;
    }
}

TEST_F(PointShadowFilterComputeTest, CubeVerticesShareTheThreeIncidentFacesSymmetrically)
{
    for (uint32_t tileResolution : {kAtlasResolution, kAtlasResolution / 4})
    {
        std::vector<Query> queries;
        for (uint32_t face = 0; face < kFaceCount; ++face)
            for (float horizontal : {0.0f, 1.0f})
                for (float vertical : {0.0f, 1.0f})
                {
                    const Query vertex = MakeQuery(face, face, horizontal, vertical);
                    const Vector3 direction{vertex.PositionSlot.x - m_LightPosition.x,
                        vertex.PositionSlot.y - m_LightPosition.y, vertex.PositionSlot.z - m_LightPosition.z};
                    for (uint32_t litFace : {direction.x >= 0 ? 0u : 1u,
                                             direction.y >= 0 ? 2u : 3u,
                                             direction.z >= 0 ? 4u : 5u})
                    {
                        Query query = vertex;
                        query.PositionSlot.w = static_cast<float>(litFace);
                        queries.push_back(query);
                    }
                }
        Dispatch(tileResolution, MakeDepths(tileResolution), queries);
        ASSERT_EQ(m_Results.size(), queries.size());
        for (size_t index = 0; index < queries.size(); ++index)
            EXPECT_NEAR(m_Results[index], 1.0f / 3.0f, 0.004f)
                << "tile=" << tileResolution << " query=" << index;
    }
}

TEST_F(PointShadowFilterComputeTest, RemappedFootprintsUseTheSelectedFacePlaneAndApplyDepthBiasOnce)
{
    for (uint32_t tileResolution : {kAtlasResolution, kAtlasResolution / 4})
        for (float inset : {-0.25f, 0.25f})
            for (float bias : {0.0f, 0.01f})
                for (bool requireBias : {false, true})
                {
                    if (requireBias && bias == 0.0f)
                        continue;
                    Query query = MakeQuery(0, 0, 1.0f - inset / tileResolution, 0.62711f);
                    query.UVFaceDepth.w += bias;
                    const Vector3 position{query.PositionSlot.x, query.PositionSlot.y, query.PositionSlot.z};
                    const uint32_t face = SelectFace(position - m_LightPosition);
                    const Vector4 clip = m_Geometry.lightVP[face].Transform(
                        Vector4{position.x, position.y, position.z, 1});
                    const float referenceDepth = clip.z / clip.w + bias;
                    const float horizontal = clip.x / clip.w * 0.5f + 0.5f;
                    const float vertical = 0.5f - clip.y / clip.w * 0.5f;
                    const float texelHorizontal = horizontal * tileResolution - 0.5f;
                    const float texelVertical = vertical * tileResolution - 0.5f;
                    const float horizontalBase = std::floor(texelHorizontal);
                    const float verticalBase = std::floor(texelVertical);
                    const float horizontalFraction = texelHorizontal - horizontalBase;
                    const float verticalFraction = texelVertical - verticalBase;
                    std::vector<float> depths(kAtlasResolution * kAtlasResolution * kLayerCount, 0.0f);
                    float expected = 1.0f;
                    for (uint32_t cornerVertical = 0; cornerVertical < 2; ++cornerVertical)
                        for (uint32_t cornerHorizontal = 0; cornerHorizontal < 2; ++cornerHorizontal)
                        {
                            const float cornerU = (horizontalBase + cornerHorizontal + 0.5f) / tileResolution;
                            const float cornerV = (verticalBase + cornerVertical + 0.5f) / tileResolution;
                            if (cornerU >= 0 && cornerU <= 1 && cornerV >= 0 && cornerV <= 1)
                                continue;
                            const Vector3 corner = Unproject(face, cornerU, cornerV, clip.z / clip.w);
                            const uint32_t neighbor = SelectFace(corner - m_LightPosition);
                            const Vector4 neighborClip = m_Geometry.lightVP[neighbor].Transform(
                                Vector4{corner.x, corner.y, corner.z, 1});
                            const float cornerReference = neighborClip.z / neighborClip.w + bias;
                            ASSERT_LT(cornerReference, referenceDepth - 0.0001f);
                            const float occluderDepth = requireBias ? cornerReference - bias * 0.5f :
                                (cornerReference + referenceDepth) * 0.5f;
                            const auto first = depths.begin() + neighbor * kAtlasResolution * kAtlasResolution;
                            std::fill(first, first + kAtlasResolution * kAtlasResolution, occluderDepth);
                            if (!requireBias)
                                expected -= (cornerHorizontal ? horizontalFraction : 1.0f - horizontalFraction) *
                                    (cornerVertical ? verticalFraction : 1.0f - verticalFraction);
                        }
                    if (!requireBias)
                    {
                        ASSERT_GT(expected, 0.5f);
                        ASSERT_LT(expected, 0.9f);
                    }
                    Dispatch(tileResolution, depths, {query}, bias);
                    ASSERT_EQ(m_Results.size(), 1u);
                    EXPECT_NEAR(m_Results[0], expected, 0.004f)
                        << "tile=" << tileResolution << " inset=" << inset << " bias=" << bias
                        << " requiresBias=" << requireBias;
                }
}

TEST_F(PointShadowFilterComputeTest, TwelveTapFilterMatchesGeometricFootprintsAtTheAuthoredRadius)
{
    constexpr float kFilterRadius = 1.5f / 1024.0f;
    constexpr uint32_t kTapCount = 12;
    constexpr float kGoldenAngle = 2.39996323f;
    for (uint32_t tileResolution : {kAtlasResolution, kAtlasResolution / 4})
    {
        std::vector<Query> queries;
        std::vector<float> expected;
        for (uint32_t edge = 0; edge < 4; ++edge)
        {
            const float inset = 0.03125f / tileResolution;
            const float horizontal = edge < 2 ? (edge == 0 ? inset : 1.0f - inset) : 0.37123f;
            const float vertical = edge >= 2 ? (edge == 2 ? inset : 1.0f - inset) : 0.62711f;
            Query query = MakeQuery(0, 0, horizontal, vertical);
            query.LightPositionMode.w = 1.0f;
            queries.push_back(query);
            const Vector4 clip = m_Geometry.lightVP[0].Transform(
                Vector4{query.PositionSlot.x, query.PositionSlot.y, query.PositionSlot.z, 1});
            const float faceU = clip.x / clip.w * 0.5f + 0.5f;
            const float faceV = 0.5f - clip.y / clip.w * 0.5f;
            const float seed = std::floor(faceU * tileResolution) * 0.06711056f +
                std::floor(faceV * tileResolution) * 0.00583715f;
            const float hash = 52.9829189f * (seed - std::floor(seed));
            const float angle = (hash - std::floor(hash)) * 6.2831853f;
            float visibility = 0.0f;
            for (uint32_t tap = 0; tap < kTapCount; ++tap)
            {
                const float radius = std::sqrt((tap + 0.5f) / kTapCount) * kFilterRadius;
                const float theta = tap * kGoldenAngle;
                const float offsetU = radius * (std::cos(theta) * std::cos(angle) -
                                                std::sin(theta) * std::sin(angle));
                const float offsetV = radius * (std::cos(theta) * std::sin(angle) +
                                                std::sin(theta) * std::cos(angle));
                const Vector3 tapPosition = Unproject(0, faceU + offsetU, faceV + offsetV, clip.z / clip.w);
                visibility += ConstantFaceVisibility(tapPosition, tileResolution, 0);
            }
            expected.push_back(visibility / kTapCount);
        }
        Dispatch(tileResolution, MakeDepths(tileResolution), queries);
        ASSERT_EQ(m_Results.size(), queries.size());
        for (size_t index = 0; index < queries.size(); ++index)
            EXPECT_NEAR(m_Results[index], expected[index], 0.004f)
                << "tile=" << tileResolution << " edge=" << index;
    }
}
} // namespace
