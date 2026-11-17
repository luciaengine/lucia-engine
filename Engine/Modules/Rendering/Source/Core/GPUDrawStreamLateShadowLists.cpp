#include "Rendering/Core/GPUDrawStreamBuilder.h"

#include "GPUDrawStreamInternal.h"
#include "Logger/Logger.h"
#include "Memory/ScratchScope.h"

#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

namespace
{

// The storage-buffer bindings of draw_stream_late_shadow_lists.comp set 0.
constexpr uint32_t kLateShadowListBindingCount = 7;
constexpr uint32_t kLateShadowListGroupSize = 64;
constexpr uint32_t kLateShadowListModeLists = 0;
constexpr uint32_t kLateShadowListModeCommands = 1;
constexpr uint32_t kCommandWords = kDrawCommandStride / sizeof(uint32_t);
// The first late list buffer holds 4 pages of a 4k-instance view without growth.
constexpr uint64_t kInitialLateShadowListWords = 1u << 14;

} // namespace

GPUDrawStreamBuilder::LateShadowListLayout GPUDrawStreamBuilder::MakeLateShadowListLayout(
    uint32_t base, uint32_t pageCount, uint32_t rowCount, uint32_t runCount, uint32_t wordsPerPage)
{
    const uint64_t pages = pageCount;
    uint64_t cursor = base;
    LateShadowListLayout layout{};
    const auto claim = [&cursor](uint64_t words)
    {
        const uint64_t start = cursor;
        cursor += words;
        return static_cast<uint32_t>(std::min<uint64_t>(start, std::numeric_limits<uint32_t>::max()));
    };
    layout.CountsBase = claim(pages * rowCount);
    layout.RunCountsBase = claim(pages * runCount);
    layout.CommandsBase = claim(pages * rowCount * kCommandWords);
    layout.WordsBase = claim(pages * wordsPerPage);
    layout.EndWord = cursor;
    return layout;
}

DescriptorSetLayoutDesc GPUDrawStreamBuilder::MakeLateShadowListDescriptorSetLayout()
{
    // visibility, instances, meshes, batch table, row runs, mesh pool groups, lists.
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "GPUDrawLateShadowList_SetLayout";
    for (uint32_t binding = 0; binding < kLateShadowListBindingCount; ++binding)
    {
        DescriptorBinding entry{};
        entry.binding = binding;
        entry.type = DescriptorType::StorageBuffer;
        entry.count = 1;
        entry.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(entry);
    }
    return layout;
}

PipelineHandle GPUDrawStreamBuilder::GetOrCreateLateShadowListPipeline()
{
    if (!m_Device)
        return {};
    std::lock_guard<std::mutex> lock(m_ScatterInitMutex);
    static_assert(sizeof(LateShadowListPushConstants) == 64,
                  "LateShadowListPushConstants must match draw_stream_late_shadow_lists.comp");
    if (!m_LateShadowListPipeline.IsValid())
        m_LateShadowListPipeline = CreateStorageComputePipeline(
            "Shaders/draw_stream_late_shadow_lists.shaderpkg", "GPUDrawStreamLateShadowLists",
            MakeLateShadowListDescriptorSetLayout(), sizeof(LateShadowListPushConstants),
            m_LateShadowListFailureLogged);
    return m_LateShadowListPipeline;
}

bool GPUDrawStreamBuilder::EnsureLateShadowListBuffer(uint64_t words)
{
    if (m_LateShadowLists.IsValid() && m_LateShadowListCursor + words <= m_LateShadowListWords)
        return true;
    if (!m_Device || !m_Device->GetCapabilities().supportsBufferDeviceAddress)
        return false;
    // Size for this frame's whole use, so the next frame does not grow again.
    const uint64_t need = m_LateShadowListCursor + words;
    const uint64_t capacity = std::max(need + need / 2u, kInitialLateShadowListWords);
    if (capacity > std::numeric_limits<uint32_t>::max())
        return false;
    if (m_LateShadowLists.IsValid())
        Logger::Log::Info("[GPUDrawStream] growing late shadow lists: {} -> {} words", m_LateShadowListWords,
                          capacity);
    BufferDesc descriptor{};
    descriptor.size = static_cast<size_t>(capacity) * sizeof(uint32_t);
    descriptor.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::Indirect)
                       | static_cast<uint32_t>(BufferUsage::TransferDst)
                       | static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress);
    descriptor.memoryUsage = BufferMemoryUsage::DeviceLocal;
    descriptor.debugName = "GPUDrawStream.LateShadowLists";
    BufferHandle buffer{};
    try
    {
        buffer = m_Device->CreateBuffer(descriptor);
    }
    catch (const std::exception& error)
    {
        Logger::Log::Error("[GPUDrawStream] late shadow list buffer creation failed: {}", error.what());
        return false;
    }
    if (!buffer.IsValid())
        return false;
    // Earlier calls of this frame published ranges into the old buffer. Retirement keeps it alive
    // until those draws retire, and this call starts at the new buffer's first word.
    RetireBuffer(m_LateShadowLists);
    m_LateShadowLists = buffer;
    m_LateShadowListWords = capacity;
    m_LateShadowListCursor = 0;
    m_LateShadowListAddress = m_Device->GetBufferDeviceAddress(buffer);
    return m_LateShadowListAddress != 0;
}

struct GPUDrawStreamBuilder::LateShadowListPass
{
    PipelineHandle Pipeline{};
    const DescriptorSetDesc* SetDesc = nullptr;
    BufferHandle Lists{};
    RenderGraph::RGBuffer Visibility{};
    RenderGraph::RGBuffer Instances{};
    RenderGraph::RGBuffer Meshes{};
    RenderGraph::RGUploadRing::Alloc Table{};
    size_t TableBytes = 0;
    RenderGraph::RGUploadRing::Alloc RowRuns{};
    size_t RowRunBytes = 0;
    RenderGraph::RGUploadRing::Alloc MeshGroups{};
    size_t MeshGroupBytes = 0;
    size_t VisibilityBytes = 0;
    size_t InstanceBytes = 0;
    size_t MeshBytes = 0;
    LateShadowListLayout Layout{};
    LateShadowListPushConstants Constants{};
    DescriptorSetHandle Set{};

    bool Prepare(RenderGraph::RGPrepareContext& context)
    {
        Set = {};
        IDevice* device = context.GetDevice();
        const BufferHandle visibility = context.GetBuffer(Visibility);
        const BufferHandle instances = context.GetBuffer(Instances);
        const BufferHandle meshes = context.GetBuffer(Meshes);
        if (!device || !visibility.IsValid() || !instances.IsValid() || !meshes.IsValid() || !Pipeline.IsValid())
            return false;
        const DescriptorSetHandle set = device->CreateDescriptorSet(*SetDesc);
        if (!set.IsValid())
            return false;
        // Without draw consolidation the shader reads neither map, so the table stands in for both.
        const bool grouped = Constants.meshGroupMode != 0u;
        const std::array<DescriptorWrite, kLateShadowListBindingCount> writes{
            StorageBufferWrite(0, visibility, 0, VisibilityBytes),
            StorageBufferWrite(1, instances, 0, InstanceBytes),
            StorageBufferWrite(2, meshes, 0, MeshBytes),
            StorageBufferWrite(3, Table.Buffer, Table.Offset, TableBytes),
            grouped ? StorageBufferWrite(4, RowRuns.Buffer, RowRuns.Offset, RowRunBytes)
                    : StorageBufferWrite(4, Table.Buffer, Table.Offset, TableBytes),
            grouped ? StorageBufferWrite(5, MeshGroups.Buffer, MeshGroups.Offset, MeshGroupBytes)
                    : StorageBufferWrite(5, Table.Buffer, Table.Offset, TableBytes),
            StorageBufferWrite(6, Lists, 0, static_cast<size_t>(Layout.EndWord) * sizeof(uint32_t)),
        };
        device->WriteDescriptorSet(set, writes);
        Set = set;
        return true;
    }

    void Record(RenderGraph::RGContext& context) const
    {
        if (!Set.IsValid())
            return;
        // Counts and command blocks start at zero. A zero command draws nothing, which is what an
        // emulated count path issues past the packed draws.
        context.Cmd->FillBuffer(Lists, static_cast<size_t>(Layout.CountsBase) * sizeof(uint32_t),
                                static_cast<size_t>(Layout.WordsBase - Layout.CountsBase) * sizeof(uint32_t), 0u);
        context.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::Transfer), static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::TransferWrite),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead) | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)));
        context.Cmd->SetPipeline(Pipeline);
        context.Cmd->BindDescriptorSet(0, Set, Pipeline);

        LateShadowListPushConstants lists = Constants;
        lists.mode = kLateShadowListModeLists;
        context.Cmd->SetPushConstants(lists);
        context.Cmd->Dispatch((Constants.instanceCount + kLateShadowListGroupSize - 1u) / kLateShadowListGroupSize, 1, 1);

        context.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader), static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead) | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite)));
        LateShadowListPushConstants commands = Constants;
        commands.mode = kLateShadowListModeCommands;
        context.Cmd->SetPushConstants(commands);
        context.Cmd->Dispatch((Constants.rowCount + kLateShadowListGroupSize - 1u) / kLateShadowListGroupSize,
                              Constants.pageCount, 1);
        context.Cmd->Barrier(CreateStreamReadyBarrier());
    }
};

RenderGraph::RGBuffer GPUDrawStreamBuilder::ScheduleLateShadowLists(
    RenderGraph::RGFrame& frame, const LateShadowListRequest& request, RenderGraph::RGBuffer instances,
    RenderGraph::RGBuffer meshes, const BatchRegistry& registry, std::span<const uint8_t> materialDepthClass,
    std::span<const uint32_t> meshPoolOrdered, std::span<const uint32_t> orderedToGroup, uint32_t instanceCount,
    uint32_t meshTableCount)
{
    const uint32_t pageCount = static_cast<uint32_t>(request.CullingIndices.size());
    if (pageCount == 0u || pageCount > kMaxLateShadowPages || instanceCount == 0u ||
        request.VisibilityStride < instanceCount || !request.Visibility.IsValid() || !instances.IsValid() ||
        !meshes.IsValid() || !m_SentinelBuffer.IsValid())
        return {};
    const PipelineHandle pipeline = GetOrCreateLateShadowListPipeline();
    if (!pipeline.IsValid())
        return {};
    assert(meshPoolOrdered.empty() == orderedToGroup.empty() && "pass both consolidation spans or neither");
    const bool grouped = !meshPoolOrdered.empty() && !orderedToGroup.empty();

    Memory::ScratchScope scratch;
    uint32_t snapshotRecords = 0u;
    const std::span<const BatchTableEntry> table = BuildShadowBatchTable(registry, materialDepthClass,
        meshPoolOrdered, scratch.AllocateArrayForOverwrite<BatchTableEntry>(BatchTableCapacity(registry)),
        scratch.Resource(), &snapshotRecords);
    if (table.empty() || snapshotRecords == 0u)
        return {};
    const uint32_t rowCount = static_cast<uint32_t>(table.size());

    // A consolidated run draws as one range. Without consolidation every row is its own run.
    TableRunLayout runs = grouped ? BuildTableRuns(table, orderedToGroup) : TableRunLayout{};
    if (!grouped)
        for (uint32_t row = 0; row < rowCount; ++row)
            runs.runs.push_back({row, table[row].capacity, 1u});
    const uint32_t runCount = static_cast<uint32_t>(runs.runs.size());

    const uint64_t words = MakeLateShadowListLayout(0u, pageCount, rowCount, runCount, snapshotRecords).EndWord;
    if (!EnsureLateShadowListBuffer(words))
        return {};
    const LateShadowListLayout layout = MakeLateShadowListLayout(
        static_cast<uint32_t>(m_LateShadowListCursor), pageCount, rowCount, runCount, snapshotRecords);
    if (layout.EndWord > m_LateShadowListWords)
        return {};

    auto* pass = frame.AllocatePassData<LateShadowListPass>();
    const auto upload = [&frame](const void* source, size_t bytes)
    {
        const RenderGraph::RGUploadRing::Alloc allocation = frame.AllocUpload(bytes);
        if (allocation.Valid())
            std::memcpy(allocation.Ptr, source, bytes);
        return allocation;
    };
    pass->TableBytes = table.size_bytes();
    pass->Table = upload(table.data(), pass->TableBytes);
    if (!pass->Table.Valid())
        return {};
    if (grouped)
    {
        pass->RowRunBytes = runs.rowRun.size() * sizeof(runs.rowRun[0]);
        pass->RowRuns = upload(runs.rowRun.data(), pass->RowRunBytes);
        pass->MeshGroupBytes = meshPoolOrdered.size_bytes();
        pass->MeshGroups = upload(meshPoolOrdered.data(), pass->MeshGroupBytes);
        if (!pass->RowRuns.Valid() || !pass->MeshGroups.Valid())
            return {};
    }
    m_LateShadowListCursor = layout.EndWord;

    // Publish one range per (page, run). The commands of a run are packed from its first row's slot
    // and its run count is the indirect draw count.
    for (uint32_t page = 0; page < pageCount; ++page)
    {
        const uint8_t cascade = request.CullingIndices[page];
        bool published = false;
        for (uint32_t runIndex = 0; runIndex < runCount; ++runIndex)
        {
            const TableRun& run = runs.runs[runIndex];
            const BatchTableEntry& first = table[run.startRow];
            const bool mirrored = (first.meshIndex & kMeshParityBit) != 0u;
            const uint32_t mesh = first.meshIndex & ~kMeshParityBit;
            const uint32_t meshKey = !grouped ? mesh : (mesh < orderedToGroup.size() ? orderedToGroup[mesh] : kAbsentPoolGroup);
            BatchDrawRange& range = GetOrInsertRange(
                MakeStreamKey(request.ViewId, cascade, first.materialIndex, meshKey, SlicePhase::A));
            range.recordBuffer = m_LateShadowLists;
            range.countBuffer = m_LateShadowLists;
            range.indirectionAddress = m_LateShadowListAddress;
            BatchDrawSegment segment{};
            segment.cmdByteOffset =
                (static_cast<size_t>(layout.CommandsBase) + (static_cast<size_t>(page) * rowCount + run.startRow) * kCommandWords)
                * sizeof(uint32_t);
            segment.countByteOffset =
                (static_cast<size_t>(layout.RunCountsBase) + static_cast<size_t>(page) * runCount + runIndex) * sizeof(uint32_t);
            segment.maxDrawCount = run.rowCount;
            (mirrored ? range.mirrored : range.even) = segment;
            published |= range.IsValid();
        }
        if (published)
            m_PublishedSliceKeys.Insert(MakeStreamKey(request.ViewId, cascade, 0u, 0u, SlicePhase::A));
    }

    pass->Pipeline = pipeline;
    pass->SetDesc = &m_LateShadowListSetDesc;
    pass->Lists = m_LateShadowLists;
    pass->Visibility = request.Visibility;
    pass->Instances = instances;
    pass->Meshes = meshes;
    pass->VisibilityBytes = (static_cast<size_t>(pageCount - 1u) * request.VisibilityStride + instanceCount) * sizeof(uint32_t);
    pass->InstanceBytes = static_cast<size_t>(instanceCount) * sizeof(GPUInstance);
    pass->MeshBytes = static_cast<size_t>(std::max(meshTableCount, 1u)) * sizeof(GPUMesh);
    pass->Layout = layout;
    LateShadowListPushConstants& constants = pass->Constants;
    constants.instanceCount = instanceCount;
    constants.pageCount = pageCount;
    constants.rowCount = rowCount;
    constants.runCount = runCount;
    constants.visibilityStride = request.VisibilityStride;
    constants.viewWorldKey = request.ViewWorldKey;
    constants.meshGroupMode = grouped ? 1u : 0u;
    constants.countsBase = layout.CountsBase;
    constants.runCountsBase = layout.RunCountsBase;
    constants.commandsBase = layout.CommandsBase;
    constants.wordsBase = layout.WordsBase;
    constants.wordsPerPage = snapshotRecords;

    const RenderGraph::RGBuffer ordering = frame.ImportExternalBuffer("DrawStream.Ordering", m_SentinelBuffer);
    frame.AddPass(
        "GPUDrawStream.LateShadowLists", PassPhase::kEarlySetup,
        [&](RenderGraph::RGPassBuilder& builder)
        {
            builder.Read(request.Visibility);
            builder.Read(instances);
            builder.Read(meshes);
            builder.Write(ordering);
            builder.Write(ordering, RenderGraph::RGBufferWrite::CopyDst);
            builder.Prepare([pass](RenderGraph::RGPrepareContext& context) { return pass->Prepare(context); });
        },
        [pass](RenderGraph::RGContext& context) { pass->Record(context); });
    return ordering;
}

} // namespace Rendering
} // namespace GameEngine
