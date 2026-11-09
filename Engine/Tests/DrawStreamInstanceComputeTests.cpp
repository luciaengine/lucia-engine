#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace GameEngine::Rendering
{
namespace
{
using TableEntry = GPUDrawStreamBuilder::BatchTableEntry;
using Constants = GPUDrawStreamBuilder::GroupCompactPushConstants;

struct DrawRecord
{
    uint32_t IndexCount = 3;
    uint32_t InstanceCount = 1;
    uint32_t FirstIndex = 0;
    int32_t VertexOffset = 0;
    uint32_t FirstInstance = 0;
};
static_assert(sizeof(DrawRecord) == 20);

struct CompactionResult
{
    std::vector<DrawRecord> Commands;
    std::vector<uint32_t> Indirection;
    std::vector<uint32_t> Cursors;
    BufferHandle CommandBuffer;
    BufferHandle IndirectionBuffer;
    BufferHandle CursorBuffer;
    uint32_t CommandBase = 0;
    uint32_t CountBase = 0;
};

void CopyPublishedRanges(RenderGraph::RGContext& context,
                         std::span<const GPUDrawStreamBuilder::BatchDrawRange> ranges,
                         BufferHandle readback, uint32_t capacity)
{
    context.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
        static_cast<uint64_t>(PipelineStageMask::ComputeShader),
        static_cast<uint64_t>(PipelineStageMask::Transfer),
        static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
        static_cast<uint64_t>(ResourceAccessMask::TransferRead)));
    for (size_t index = 0; index < ranges.size(); ++index)
    {
        const auto& range = ranges[index];
        const size_t destination = index * (sizeof(uint32_t) + capacity * sizeof(DrawRecord));
        context.Cmd->CopyBuffer(range.countBuffer, readback, sizeof(uint32_t),
            range.even.countByteOffset, destination);
        context.Cmd->CopyBuffer(range.recordBuffer, readback,
            std::min(capacity, range.even.maxDrawCount) * sizeof(DrawRecord),
            range.even.cmdByteOffset, destination + sizeof(uint32_t));
    }
}

class DrawStreamInstanceComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = ::CreateVulkanDeviceFast();
        if (!m_Device)
        {
            GTEST_SKIP() << "No graphics device available";
        }
        m_Layout = GPUDrawStreamBuilder::MakeGroupCompactDescriptorSetLayout();
        m_InstancePipeline = LoadPipeline("Shaders/draw_stream_instance_compact.shaderpkg");
        m_GroupPipeline = LoadPipeline("Shaders/draw_stream_group_compact.shaderpkg");
        ASSERT_TRUE(m_InstancePipeline.IsValid());
        ASSERT_TRUE(m_GroupPipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (BufferHandle buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        GPUDrawStreamBuilder::SetShaderLoader(nullptr);
        m_Device->Shutdown();
    }

    PipelineHandle LoadPipeline(const char* path)
    {
        ShaderPackage package{};
        std::string error;
        if (!LoadShaderPkg(path, m_Device->PreferredShaderSource(), package, &error))
        {
            ADD_FAILURE() << path << ": " << error;
            return {};
        }
        auto stage = package.stageBytes.find("cs");
        if (stage == package.stageBytes.end())
        {
            ADD_FAILURE() << "Missing compute stage: " << path;
            return {};
        }
        ComputePipelineDesc description{};
        description.ComputeShader = std::make_shared<const std::vector<uint8_t>>(stage->second);
        description.DescriptorSetLayouts = {m_Device->InternDescriptorSetLayout(m_Layout)};
        description.PushConstants.Size = sizeof(Constants);
        description.PushConstants.StageMask = kShaderStageCompute;
        return m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(description));
    }

    template <typename Value>
    BufferHandle Upload(const std::vector<Value>& values)
    {
        BufferDesc description{};
        description.size = values.size() * sizeof(Value);
        description.usage = static_cast<uint32_t>(BufferUsage::Storage)
            | static_cast<uint32_t>(BufferUsage::Indirect)
            | static_cast<uint32_t>(BufferUsage::TransferDst);
        description.memoryUsage = BufferMemoryUsage::Readback;
        auto buffer = m_Device->CreateBuffer(description);
        EXPECT_TRUE(buffer.IsValid());
        m_Buffers.push_back(buffer);
        void* mapped = m_Device->MapBuffer(buffer);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(mapped, values.data(), description.size);
        m_Device->UnmapBuffer(buffer);
        return buffer;
    }

    template <typename Value>
    std::vector<Value> Read(BufferHandle buffer, size_t count)
    {
        std::vector<Value> values(count);
        void* mapped = m_Device->MapBuffer(buffer);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(values.data(), mapped, count * sizeof(Value));
        m_Device->UnmapBuffer(buffer);
        return values;
    }

    CompactionResult Run(const std::vector<TableEntry>& table, const std::vector<uint32_t>& live,
                         const std::vector<DrawRecord>& source, const std::vector<uint32_t>& words,
                         bool aggregate = true, bool paired = false, bool clearUnused = false,
                         std::vector<uint32_t> rowRuns = {})
    {
        const uint32_t rowCount = static_cast<uint32_t>(table.size());
        const uint32_t capacity = static_cast<uint32_t>(source.size());
        constexpr uint32_t kCursorBase = 3;
        constexpr uint32_t kSourceBase = 11;
        const uint32_t destinationBase = kSourceBase + capacity + 7;
        const uint32_t bufferSlots = destinationBase + capacity + 9;
        const uint32_t commandCursorBase = kCursorBase + rowCount;
        const uint32_t runCursorBase = commandCursorBase + rowCount;
        std::vector<uint32_t> cursorSeed(runCursorBase + rowCount, 0);
        std::copy(live.begin(), live.end(), cursorSeed.begin() + kCursorBase);
        if (rowRuns.empty())
        {
            for (uint32_t row = 0; row < rowCount; ++row)
            {
                rowRuns.push_back(row);
                rowRuns.push_back(row);
            }
        }
        std::vector<DrawRecord> records(bufferSlots, DrawRecord{0, 0, 0, 0, 0});
        std::vector<uint32_t> indirection(bufferSlots, 0xDEADBEEFu);
        std::copy(source.begin(), source.end(), records.begin() + kSourceBase);
        std::copy(words.begin(), words.end(), indirection.begin() + kSourceBase);
        for (uint32_t index = 0; index < capacity; ++index)
            records[kSourceBase + index].FirstInstance = kSourceBase + index;
        const auto tableBuffer = Upload(table);
        const auto runBuffer = Upload(rowRuns);
        const auto cursors = Upload(cursorSeed);
        const auto commandBuffer = Upload(records);
        const auto indexBuffer = Upload(indirection);
        DescriptorSetDesc description{};
        description.layout = m_Layout;
        description.transient = true;
        auto set = m_Device->CreateDescriptorSet(description);
        const std::vector<std::pair<BufferHandle, size_t>> bindings = {
            {tableBuffer, table.size() * sizeof(TableEntry)},
            {runBuffer, rowRuns.size() * sizeof(uint32_t)},
            {cursors, cursorSeed.size() * sizeof(uint32_t)},
            {commandBuffer, records.size() * sizeof(DrawRecord)},
            {indexBuffer, indirection.size() * sizeof(uint32_t)}};
        for (uint32_t binding = 0; binding < bindings.size(); ++binding)
            m_Device->UpdateStorageBufferBinding(set, binding, bindings[binding].first, 0,
                                                bindings[binding].second);
        Constants constants{};
        constants.recordCount = capacity;
        constants.rowCount = rowCount;
        constants.cursorBase = kCursorBase;
        constants.countBase = commandCursorBase;
        constants.srcRecordBase = kSourceBase;
        constants.dstRecordBase = destinationBase;
        constants.pairedRecords = paired ? 1u : 0u;
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        commands->Begin();
        if (aggregate)
        {
            commands->SetPipeline(m_InstancePipeline);
            commands->BindDescriptorSet(0, set, m_InstancePipeline);
            commands->SetPushConstants(constants);
            commands->Dispatch((capacity + 63) / 64, 1, 1);
            if (clearUnused)
            {
                commands->Barrier(ResourceBarrier::CreateMemoryBarrier(
                    static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                    static_cast<uint64_t>(PipelineStageMask::Transfer),
                    static_cast<uint64_t>(ResourceAccessMask::ShaderRead)
                        | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                    static_cast<uint64_t>(ResourceAccessMask::TransferWrite)));
                commands->FillBuffer(commandBuffer, kSourceBase * sizeof(DrawRecord),
                                     capacity * sizeof(DrawRecord), 0);
            }
            commands->Barrier(ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::ComputeShader)
                    | static_cast<uint64_t>(PipelineStageMask::Transfer),
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)
                    | static_cast<uint64_t>(ResourceAccessMask::TransferWrite),
                static_cast<uint64_t>(ResourceAccessMask::ShaderRead)
                    | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)));
            constants.cursorBase = commandCursorBase;
            std::swap(constants.srcRecordBase, constants.dstRecordBase);
            constants.pairedRecords = 0;
            constants.InstancedRecords = 1;
        }
        constants.countBase = runCursorBase;
        commands->SetPipeline(m_GroupPipeline);
        commands->BindDescriptorSet(0, set, m_GroupPipeline);
        commands->SetPushConstants(constants);
        commands->Dispatch((capacity + 63) / 64, 1, 1);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        CompactionResult result;
        result.Commands = Read<DrawRecord>(commandBuffer, bufferSlots);
        result.Indirection = Read<uint32_t>(indexBuffer, bufferSlots);
        result.Cursors = Read<uint32_t>(cursors, cursorSeed.size());
        result.CommandBuffer = commandBuffer;
        result.IndirectionBuffer = indexBuffer;
        result.CursorBuffer = cursors;
        result.CommandBase = constants.dstRecordBase;
        result.CountBase = runCursorBase;
        return result;
    }

    using InstanceGeometry = std::tuple<uint32_t, uint32_t, int32_t, uint32_t>;

    std::multiset<InstanceGeometry> Expand(const CompactionResult& result,
                                          const std::vector<TableEntry>& table)
    {
        std::multiset<InstanceGeometry> values;
        for (uint32_t row = 0; row < table.size(); ++row)
        {
            const uint32_t count = result.Cursors[result.CountBase + row];
            EXPECT_LE(count, table[row].capacity);
            for (uint32_t index = 0; index < std::min(count, table[row].capacity); ++index)
            {
                const auto& command = result.Commands[result.CommandBase + table[row].recordOffset + index];
                EXPECT_GT(command.InstanceCount, 0u);
                const uint64_t end = uint64_t{command.FirstInstance} + command.InstanceCount;
                EXPECT_LE(end, result.Indirection.size());
                if (end > result.Indirection.size())
                    continue;
                for (uint32_t instance = 0; instance < command.InstanceCount; ++instance)
                    values.emplace(command.IndexCount, command.FirstIndex, command.VertexOffset,
                                   result.Indirection[command.FirstInstance + instance]);
            }
        }
        return values;
    }

    std::vector<uint8_t> Raster(const CompactionResult& draws, uint32_t capacity)
    {
        constexpr uint32_t kWidth = 160, kHeight = 100, kInstances = 72;
        std::vector<GPUInstance> instances(kInstances);
        for (uint32_t index = 0; index < instances.size(); ++index)
        {
            instances[index].transform = Mathematics::Matrix4x4::Translation(
                Mathematics::Vector3(-.875f + .25f * (index % 8),
                                     -.8f + .2f * (index / 8), 0));
            instances[index].materialIndex = index;
            instances[index].custom0 = Mathematics::Vector4(
                static_cast<float>(index + 1) / kInstances, .25f, .5f, .4f);
        }
        const auto instanceBuffer = Upload(instances);
        TextureDesc texture{};
        texture.width = kWidth;
        texture.height = kHeight;
        texture.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        texture.usage = static_cast<uint32_t>(TextureUsage::TransferSrc)
            | static_cast<uint32_t>(TextureUsage::RenderTarget);
        const auto target = m_Device->CreateTexture(texture);
        EXPECT_TRUE(target.IsValid());
        DescriptorSetLayoutDesc layout{};
        for (uint32_t binding = 0; binding < 2; ++binding)
        {
            DescriptorBinding entry{};
            entry.binding = binding;
            entry.type = DescriptorType::StorageBuffer;
            entry.count = 1;
            entry.shaderStages = kShaderStageVertex;
            layout.bindings.push_back(entry);
        }
        PipelineDesc description{};
        description.vertexShader = Utils::LoadShaderFile("Shaders/render_layer_probe.vert.spv");
        description.pixelShader = Utils::LoadShaderFile("Shaders/render_layer_probe.frag.spv");
        description.colorAttachmentFormats = {texture.format};
        description.colorBlendState.attachments.resize(1);
        description.rasterizationState.cullMode = CullModeFlagBits::None;
        description.descriptorSetLayouts = {layout};
        const auto pipeline = m_Device->CreatePipeline(description);
        EXPECT_TRUE(pipeline.IsValid());
        if (!pipeline.IsValid())
        {
            m_Device->DestroyTexture(target);
            return {};
        }
        DescriptorSetDesc setDescription{};
        setDescription.layout = layout;
        setDescription.transient = true;
        const auto set = m_Device->CreateDescriptorSet(setDescription);
        m_Device->UpdateStorageBufferBinding(set, 0, instanceBuffer, 0,
                                            instances.size() * sizeof(GPUInstance));
        m_Device->UpdateStorageBufferBinding(set, 1, draws.IndirectionBuffer, 0,
                                            draws.Indirection.size() * sizeof(uint32_t));
        BufferDesc indexDescription{};
        indexDescription.size = 3 * sizeof(uint32_t);
        indexDescription.usage = static_cast<uint32_t>(BufferUsage::Index);
        indexDescription.memoryUsage = BufferMemoryUsage::Upload;
        const auto indices = m_Device->CreateBuffer(indexDescription);
        m_Buffers.push_back(indices);
        const uint32_t triangle[] = {0, 1, 2};
        void* mapped = m_Device->MapBuffer(indices);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(mapped, triangle, sizeof(triangle));
        m_Device->UnmapBuffer(indices);
        const auto readback = m_Device->CreateReadbackBuffer(kWidth * kHeight * 4);
        m_Buffers.push_back(readback);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateBufferBarrier(
            draws.CommandBuffer, ResourceState::UnorderedAccess, ResourceState::IndirectArgs));
        commands->Barrier(ResourceBarrier::CreateBufferBarrier(
            draws.CursorBuffer, ResourceState::UnorderedAccess, ResourceState::IndirectArgs));
        commands->Barrier(ResourceBarrier::CreateBufferBarrier(
            draws.IndirectionBuffer, ResourceState::UnorderedAccess, ResourceState::ShaderResource));
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(
            target, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc pass{};
        pass.colorTargets[0] = target;
        pass.colorTargetCount = 1;
        pass.clearColor[0] = true;
        pass.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        commands->BeginRenderPass(pass);
        commands->SetPipeline(pipeline);
        commands->BindDescriptorSet(0, set, pipeline);
        commands->SetViewport(0, 0, kWidth, kHeight);
        commands->SetScissor(0, 0, kWidth, kHeight);
        commands->SetIndexBuffer(indices, IndexType::Uint32);
        commands->DrawIndexedIndirectCount(draws.CommandBuffer, draws.CursorBuffer, capacity,
            sizeof(DrawRecord), draws.CommandBase * sizeof(DrawRecord),
            draws.CountBase * sizeof(uint32_t));
        commands->EndRenderPass();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(
            target, ResourceState::RenderTarget, ResourceState::CopySource));
        commands->CopyTextureToBuffer(target, readback, kWidth, kHeight);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        auto pixels = Read<uint8_t>(readback, kWidth * kHeight * 4);
        m_Device->DestroyPipeline(pipeline);
        m_Device->DestroyTexture(target);
        return pixels;
    }

    void CheckProductionSchedule(bool repeatedMeshes, bool emulateCount = false);

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout;
    PipelineHandle m_InstancePipeline;
    PipelineHandle m_GroupPipeline;
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(DrawStreamInstanceComputeTest, RepeatedGeometryReducesCommandCountWithoutLosingInstances)
{
    constexpr uint32_t kCount = 193;
    const std::vector<TableEntry> table{{7, 4, 0, kCount}};
    std::vector<DrawRecord> records(kCount);
    std::vector<uint32_t> words(kCount);
    for (uint32_t index = 0; index < kCount; ++index)
        words[index] = 1000u + index;
    const auto reference = Run(table, {kCount}, records, words, false);
    const auto result = Run(table, {kCount}, records, words);
    EXPECT_LE(result.Cursors[result.CountBase], 4u);
    const uint32_t commandCount = result.Cursors[result.CountBase];
    ASSERT_LE(commandCount, kCount);
    const auto begin = result.Commands.begin() + result.CommandBase;
    EXPECT_TRUE(std::any_of(begin, begin + commandCount,
        [](const DrawRecord& command) { return command.InstanceCount > 1; }));
    EXPECT_EQ(Expand(result, table), Expand(reference, table));
    EXPECT_EQ(result.Cursors[3], kCount);
}

TEST_F(DrawStreamInstanceComputeTest, GeometryAndRowsRemainSeparateAcrossPartialWorkgroups)
{
    const std::vector<TableEntry> table{{3, 2, 0, 23}, {3, 5, 23, 83}, {4, 5, 106, 9}};
    std::vector<DrawRecord> records(115);
    std::vector<uint32_t> words(115);
    for (uint32_t index = 0; index < records.size(); ++index)
    {
        records[index].IndexCount = index % 7 < 3 ? 6u : 12u;
        records[index].FirstIndex = index % 7 < 3 ? 17u : 71u;
        records[index].VertexOffset = index % 7 < 3 ? -4 : 9;
        words[index] = 0xA0000000u | index;
    }
    const auto reference = Run(table, {19, 67, 5}, records, words, false);
    const auto result = Run(table, {19, 67, 5}, records, words);
    EXPECT_EQ(Expand(result, table), Expand(reference, table));
    EXPECT_EQ(result.Cursors[3], 19u);
    EXPECT_EQ(result.Cursors[4], 67u);
    EXPECT_EQ(result.Cursors[5], 5u);
}

TEST_F(DrawStreamInstanceComputeTest, EmptyVisibilityEmitsNoCommandsOrInstances)
{
    const std::vector<TableEntry> table{{0, 0, 0, 65}};
    const auto result = Run(table, {0}, std::vector<DrawRecord>(65), std::vector<uint32_t>(65));
    EXPECT_EQ(result.Cursors[result.CountBase], 0u);
    EXPECT_TRUE(Expand(result, table).empty());
}

TEST_F(DrawStreamInstanceComputeTest, OverflowAndCrossfadeTailKeepOnlyCompletePairs)
{
    const std::vector<TableEntry> table{{0, 0, 0, 67}, {0, 1, 67, 10}};
    std::vector<DrawRecord> records(77);
    std::vector<uint32_t> words(77);
    for (uint32_t index = 0; index < words.size(); ++index)
        words[index] = 0xC0000000u | index;
    const auto reference = Run(table, {101, 7}, records, words, false, true);
    const auto result = Run(table, {101, 7}, records, words, true, true);
    EXPECT_EQ(Expand(result, table), Expand(reference, table));
    EXPECT_EQ(Expand(result, table).size(), 72u);
    EXPECT_EQ(result.Cursors[3], 101u);
    EXPECT_EQ(result.Cursors[4], 7u);
}

TEST_F(DrawStreamInstanceComputeTest, CountEmulationLeavesUnusedCommandsZero)
{
    const std::vector<TableEntry> table{{0, 0, 0, 129}};
    const auto result = Run(table, {129}, std::vector<DrawRecord>(129), std::vector<uint32_t>(129), true, false, true);
    const uint32_t live = result.Cursors[result.CountBase];
    EXPECT_LE(live, 3u);
    for (uint32_t index = live; index < 129; ++index)
    {
        EXPECT_EQ(result.Commands[result.CommandBase + index].IndexCount, 0u);
    }
}

TEST_F(DrawStreamInstanceComputeTest, MeshMajorRunPacksRowsWithoutCombiningTheirInstances)
{
    const std::vector<TableEntry> table{{3, 2, 0, 23}, {3, 5, 23, 83}, {4, 5, 106, 9}};
    const std::vector<TableEntry> mergedRun{{0, 0, 0, 115}};
    std::vector<uint32_t> words(115);
    for (uint32_t index = 0; index < words.size(); ++index)
        words[index] = index;
    const std::vector<uint32_t> rowRuns(6, 0);
    const auto reference = Run(table, {19, 67, 5}, std::vector<DrawRecord>(115), words,
                               false, false, false, rowRuns);
    const auto result = Run(table, {19, 67, 5}, std::vector<DrawRecord>(115), words,
                            true, false, false, rowRuns);
    EXPECT_EQ(Expand(result, mergedRun), Expand(reference, mergedRun));
    const uint32_t count = result.Cursors[result.CountBase];
    EXPECT_LE(count, 6u);
    for (uint32_t index = 0; index < std::min(count, 115u); ++index)
    {
        const auto& command = result.Commands[result.CommandBase + index];
        ASSERT_LT(command.FirstInstance, result.Indirection.size());
        const uint32_t first = result.Indirection[command.FirstInstance];
        const uint32_t end = first < 23 ? 23 : first < 106 ? 106 : 115;
        EXPECT_LE(first + command.InstanceCount, end);
    }
    EXPECT_EQ(result.Indirection.front(), 0xDEADBEEFu);
    EXPECT_EQ(result.Indirection.back(), 0xDEADBEEFu);
    EXPECT_EQ(result.Commands.front().IndexCount, 0u);
    EXPECT_EQ(result.Commands.back().IndexCount, 0u);
}

TEST_F(DrawStreamInstanceComputeTest, InstancedIndirectRasterMatchesSingleInstanceCommands)
{
    const std::vector<TableEntry> table{{0, 0, 0, 69}};
    const std::vector<DrawRecord> records(69);
    std::vector<uint32_t> words(69);
    std::iota(words.begin(), words.end(), 0u);
    const auto reference = Run(table, {67}, records, words, false);
    const auto result = Run(table, {67}, records, words);
    ASSERT_EQ(result.Cursors[result.CountBase], 2u);
    ASSERT_GT(result.Commands[result.CommandBase].InstanceCount, 1u);
    const auto baseline = Raster(reference, 69);
    const auto repeated = Raster(reference, 69);
    const auto instanced = Raster(result, 69);
    ASSERT_EQ(baseline.size(), 160u * 100u * 4u);
    EXPECT_EQ(repeated, baseline);
    EXPECT_EQ(instanced, baseline);
    for (uint32_t index = 0; index < 72; ++index)
    {
        const size_t pixel = ((90u - 10u * (index / 8)) * 160u + 10u + 20u * (index % 8)) * 4u;
        EXPECT_EQ(baseline[pixel] > 0, index < 67);
        if (index < 67)
        {
            EXPECT_NEAR(baseline[pixel], 255.0 * (index + 1) / 72, 1);
            EXPECT_NEAR(baseline[pixel + 2], 255.0 * ((index & 7u) + 1) / 8, 1);
        }
    }
}

void DrawStreamInstanceComputeTest::CheckProductionSchedule(bool repeatedMeshes, bool emulateCount)
{
    namespace RG = RenderGraph;
    constexpr uint32_t kCount = 193;
    auto& capabilities = const_cast<RenderingDeviceCapabilities&>(m_Device->GetCapabilities());
    const bool nativeCount = capabilities.supportsDrawIndirectCountNative;
    if (emulateCount)
    {
        // Force the producer's count-emulation contract on native-count hardware.
        capabilities.supportsDrawIndirectCountNative = false;
    }
    GPUDrawStreamBuilder::SetShaderLoader(nullptr);
    if (!repeatedMeshes)
        GPUDrawStreamBuilder::SetShaderLoader([](const char* path)
        {
            if (std::strcmp(path, "Shaders/draw_stream_instance_compact.shaderpkg") == 0)
                return std::vector<uint8_t>{};
            return Utils::LoadShaderFile(path);
        });
    GPUScene scene(m_Device.get());
    ASSERT_TRUE(scene.Initialize(256, 256));
    GPUMesh mesh{};
    mesh.indexCount = mesh.vertexCount = mesh.lodIndexCount[0] = 3;
    mesh.lodCount = 1;
    mesh.boundingRadius = 1;
    const auto depthClass = repeatedMeshes ? MaterialDepthClass::EligibleSingleSided
                                          : MaterialDepthClass::MaterialDependent;
    const std::vector<uint8_t> depthClasses(2, static_cast<uint8_t>(depthClass));
    const uint32_t sharedMeshIndex = repeatedMeshes ? scene.AddMesh(mesh) : 0;
    for (uint32_t index = 0; index < kCount; ++index)
    {
        GPUInstance instance{};
        instance.meshIndex = repeatedMeshes ? sharedMeshIndex : scene.AddMesh(mesh);
        instance.materialIndex = repeatedMeshes ? index % 2 : 0;
        instance.flags = DepthClassInstanceFlagBits(depthClass);
        instance.boundingRadius = 1;
        scene.AddInstance(instance);
    }
    scene.BeginFrame();
    GPUDrawStreamBuilder builder(m_Device.get());
    ASSERT_TRUE(builder.Initialize());
    RG::RGResourcePool persistent(m_Device.get());
    RG::RGTransientPool transient(m_Device.get());
    RG::RGUploadRing ring(m_Device.get(), 2, 64 * 1024);
    RG::RGFrame frame(m_Device.get(), &persistent, &transient, &ring);
    frame.BeginFrame(1);
    const auto visible = Upload(std::vector<uint32_t>(kCount, 1));
    const auto visibility = frame.ImportExternalBuffer("InstanceTest.Visible", visible);
    const auto instances = frame.ImportExternalBuffer("InstanceTest.Instances", scene.GetInstanceBuffer());
    const auto hot = frame.ImportExternalBuffer("InstanceTest.Hot", scene.GetScatterHotBuffer());
    const auto meshes = frame.ImportExternalBuffer("InstanceTest.Meshes", scene.GetMeshBuffer());
    std::vector<uint32_t> ordered(scene.GetMeshes().size());
    std::iota(ordered.begin(), ordered.end(), 0u);
    const std::vector<uint32_t> groups(ordered.size(), 7);
    builder.BeginArenaFrame();
    std::vector<GPUDrawStreamBuilder::BatchDrawRange> ranges;
    std::vector<RG::RGBuffer> sentinels;
    for (uint32_t call = 0; call < 2; ++call)
    {
        GPUDrawStreamBuilder::SliceRegistration color{};
        color.viewId = 17 + call;
        color.lod.forceLod = 0;
        builder.RegisterSlice(color);
        if (call == 0)
        {
            auto shadow = color;
            shadow.table = GPUDrawStreamBuilder::SliceTable::Shadow;
            shadow.cascadeIndex = 0;
            builder.RegisterSlice(shadow);
        }
        const auto sentinel = builder.ScheduleUnifiedScatter(frame, "InstanceTest", 0, visibility,
            instances, hot, meshes, scene.GetBatchRegistry(), depthClasses, {}, ordered, groups,
            kCount, static_cast<uint32_t>(scene.GetMeshes().size()));
        ASSERT_TRUE(sentinel.IsValid());
        sentinels.push_back(sentinel);
        for (uint32_t material = 0; material < (repeatedMeshes ? 2u : 1u); ++material)
            ranges.push_back(builder.FindBatchDrawRange(color.viewId,
                GPUDrawStreamBuilder::kCascadeIndexNone, material, 7, color.phase));
        if (call == 0)
            ranges.push_back(builder.FindBatchDrawRange(color.viewId, 0,
                repeatedMeshes ? GPUDrawStreamBuilder::kSharedDepthSingleSidedSentinel : 0,
                7, color.phase));
    }
    const auto readback = m_Device->CreateReadbackBuffer(ranges.size() * (sizeof(uint32_t) + kCount * sizeof(DrawRecord)));
    m_Buffers.push_back(readback);
    frame.AddPass("InstanceTest.ReadRanges", 1,
        [sentinels](RG::RGPassBuilder& pass)
        {
            for (const auto sentinel : sentinels)
                pass.Read(sentinel);
            pass.PreventCulling();
        },
        [ranges, readback](RG::RGContext& context) { CopyPublishedRanges(context, ranges, readback, kCount); });
    for (const auto& range : ranges)
    {
        ASSERT_TRUE(range.IsValid());
    }
    frame.Execute();
    m_Device->WaitForIdle();
    const auto bytes = Read<uint8_t>(readback, ranges.size() * (sizeof(uint32_t) + kCount * sizeof(DrawRecord)));
    for (size_t index = 0; index < ranges.size(); ++index)
    {
        SCOPED_TRACE(index);
        const size_t offset = index * (sizeof(uint32_t) + kCount * sizeof(DrawRecord));
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + offset, sizeof(count));
        ASSERT_GT(count, 0u);
        ASSERT_LE(count, ranges[index].even.maxDrawCount);
        if (repeatedMeshes)
        {
            EXPECT_LE(count, 4u);
        }
        else
        {
            EXPECT_EQ(count, kCount);
        }
        uint32_t instancesDrawn = 0;
        for (uint32_t commandIndex = 0; commandIndex < count; ++commandIndex)
        {
            DrawRecord command{};
            std::memcpy(&command, bytes.data() + offset + sizeof(uint32_t)
                + commandIndex * sizeof(DrawRecord), sizeof(command));
            EXPECT_EQ(command.IndexCount, 3u);
            EXPECT_GT(command.InstanceCount, 0u);
            if (!repeatedMeshes)
            {
                EXPECT_EQ(command.InstanceCount, 1u);
            }
            instancesDrawn += command.InstanceCount;
        }
        EXPECT_EQ(instancesDrawn, ranges[index].even.maxDrawCount);
        if (emulateCount)
        {
            for (uint32_t commandIndex = count; commandIndex < ranges[index].even.maxDrawCount; ++commandIndex)
            {
                DrawRecord command{};
                std::memcpy(&command, bytes.data() + offset + sizeof(uint32_t)
                    + commandIndex * sizeof(DrawRecord), sizeof(command));
                EXPECT_EQ(command.IndexCount, 0u);
            }
        }
    }
    capabilities.supportsDrawIndirectCountNative = nativeCount;
    builder.Shutdown();
    scene.Shutdown();
    GPUDrawStreamBuilder::SetShaderLoader(nullptr);
}

TEST_F(DrawStreamInstanceComputeTest, ProductionSchedulePublishesInstancedRangesForEverySliceAndCall)
{
    CheckProductionSchedule(true);
}

TEST_F(DrawStreamInstanceComputeTest, SingletonRowsDoNotRequireTheInstancingPipeline)
{
    CheckProductionSchedule(false);
}

TEST_F(DrawStreamInstanceComputeTest, ProductionCountEmulationLeavesUnusedCommandsZero)
{
    CheckProductionSchedule(true, true);
}

} // namespace
} // namespace GameEngine::Rendering
