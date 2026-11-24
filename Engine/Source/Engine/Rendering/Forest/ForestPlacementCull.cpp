#include "ForestPlacementCull.h"

#include "ForestPlacementGpuData.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace GameEngine::Engine::Renderer
{
using namespace Rendering;
using namespace Rendering::RenderGraph;

namespace
{
constexpr uint32 kSlotsBinding = 0;
constexpr uint32 kPagesBinding = 1;
constexpr uint32 kVisibleBinding = 2;
constexpr uint32 kArgsBinding = 3;
constexpr uint32 kCoarseVisibleBinding = 4;
constexpr uint32 kCoarseArgsBinding = 5;
constexpr uint32 kViewBinding = 0;
constexpr uint32 kWindBinding = 1;
constexpr uint32 kOcclusionBinding = 0;
constexpr uint32 kMaxCullGroups = 65535;
constexpr uint32 kSlotSize = PageStreaming::PackedPlacementStore::kPlacementsPerPage;

DescriptorBinding StorageBinding(uint32 binding, bool readOnly)
{
    return {.binding = binding, .type = DescriptorType::StorageBuffer,
            .shaderStages = kShaderStageCompute, .flags = readOnly ? kDescriptorBindingReadOnlyStorage : 0u};
}
} // namespace

bool ForestPlacementCull::Initialize(IDevice& device, bool splats)
{
    m_Splats = splats;
    ShaderPackage package{};
    std::string error;
    const char* path = splats ? "Shaders/forest_splat_cull.shaderpkg" : "Shaders/forest_placement_cull.shaderpkg";
    if (!LoadShaderPkg(path, device.PreferredShaderSource(), package, &error))
    {
        LOG_WARNING("Forest placement culling shader failed to load: {}", error);
        return false;
    }
    const auto stage = package.stageBytes.find("cs");
    if (stage == package.stageBytes.end() || stage->second.empty())
        return false;

    m_PlacementsLayout.debugName = "Forest.Cull.Placements";
    m_PlacementsLayout.bindings = {StorageBinding(kSlotsBinding, true), StorageBinding(kPagesBinding, true),
                                   StorageBinding(kVisibleBinding, false), StorageBinding(kArgsBinding, false)};
    if (splats)
    {
        m_PlacementsLayout.bindings.push_back(StorageBinding(kCoarseVisibleBinding, false));
        m_PlacementsLayout.bindings.push_back(StorageBinding(kCoarseArgsBinding, false));
    }
    m_ViewLayout.debugName = "Forest.Cull.View";
    m_ViewLayout.bindings = {{.binding = kViewBinding, .type = DescriptorType::UniformBuffer,
                             .shaderStages = kShaderStageCompute}};
    if (splats)
        m_ViewLayout.bindings.push_back({.binding = kWindBinding, .type = DescriptorType::UniformBuffer,
                                         .shaderStages = kShaderStageCompute});
    ComputePipelineDesc desc{};
    desc.DebugName = "Forest.CullPlacements";
    desc.ComputeShader = std::make_shared<const std::vector<uint8>>(std::move(stage->second));
    desc.DescriptorSetLayouts = {device.InternDescriptorSetLayout(m_PlacementsLayout),
                                 device.InternDescriptorSetLayout(m_ViewLayout)};
    m_Pipeline = device.InternComputePipeline(std::move(desc));
    if (!m_Pipeline.IsValid())
        return false;
    device.RequestComputePipeline(m_Pipeline);

    const char* occlusionPath = splats ? "Shaders/forest_splat_occlusion_cull.shaderpkg" :
                                        "Shaders/forest_placement_occlusion_cull.shaderpkg";
    ShaderPackage occlusionPackage{};
    if (LoadShaderPkg(occlusionPath, device.PreferredShaderSource(), occlusionPackage))
    {
        const auto occlusionStage = occlusionPackage.stageBytes.find("cs");
        if (occlusionStage != occlusionPackage.stageBytes.end() && !occlusionStage->second.empty())
        {
            m_OcclusionLayout.debugName = "Forest.Cull.Occlusion";
            m_OcclusionLayout.bindings = {{.binding = kOcclusionBinding, .type = DescriptorType::CombinedImageSampler,
                                           .shaderStages = kShaderStageCompute}};
            ComputePipelineDesc occlusionDesc{};
            occlusionDesc.DebugName = "Forest.CullOccludedPlacements";
            occlusionDesc.ComputeShader =
                std::make_shared<const std::vector<uint8>>(std::move(occlusionStage->second));
            occlusionDesc.DescriptorSetLayouts = {device.InternDescriptorSetLayout(m_PlacementsLayout),
                device.InternDescriptorSetLayout(m_ViewLayout), device.InternDescriptorSetLayout(m_OcclusionLayout)};
            m_OcclusionPipeline = device.InternComputePipeline(std::move(occlusionDesc));
            if (m_OcclusionPipeline.IsValid())
                device.RequestComputePipeline(m_OcclusionPipeline);
        }
    }
    return true;
}

std::optional<ForestPlacementCull::DrawBuffers> ForestPlacementCull::Declare(RGFrame& frame,
    std::string_view scope, BufferHandle slots, uint32 slotCount, const ForestPlacementGpuView& view,
    std::span<const ForestPlacementGpuPage> pages, uint32 indexCount, const ForestSplatWindGpu* wind,
    RGTexture occlusionHZB, SamplerHandle occlusionSampler, uint32 coarseIndexCount)
{
    if (!m_Pipeline.IsValid() || !slots.IsValid() || pages.empty() || pages.size() > slotCount ||
        pages.size() > kMaxCullGroups || (wind != nullptr) != m_Splats || indexCount == 0)
        return std::nullopt;
    auto* device = frame.Device();
    if (!device || device->RequestComputePipeline(m_Pipeline) != PipelineBuildState::Warm)
        return std::nullopt;
    const bool useOcclusion = occlusionHZB.IsValid() && occlusionSampler.IsValid() && m_OcclusionPipeline.IsValid() &&
        device->RequestComputePipeline(m_OcclusionPipeline) == PipelineBuildState::Warm;
    const auto cullPipeline = useOcclusion ? m_OcclusionPipeline : m_Pipeline;

    const uint64 slotBytes = uint64{slotCount} * kSlotSize * sizeof(PageStreaming::PackedPlacement);
    // Each submitted page emits at most one row per placement. Keep capacity
    // when demand drops to avoid replacing in-flight buffers during camera motion.
    const uint32 visiblePages = std::min(slotCount, std::max(m_VisiblePageCapacity,
        std::bit_ceil(static_cast<uint32>(pages.size()))));
    const uint32 visibleStride = m_Splats ? sizeof(ForestPlacementGpuVisibleSplat) : sizeof(ForestPlacementGpuVisible);
    const uint64 visibleBytes = uint64{visiblePages} * kSlotSize * visibleStride;
    const auto pageUpload = frame.AllocUpload(pages.size_bytes());
    const auto viewUpload = frame.AllocUpload(sizeof(view));
    const bool useCoarse = wind && coarseIndexCount != 0 && wind->Lod.x > 0.0f;
    const auto argsUpload = frame.AllocUpload(sizeof(ForestPlacementIndirectDraw) * (useCoarse ? 2u : 1u));
    if (!pageUpload.Valid() || !viewUpload.Valid() || !argsUpload.Valid())
        return std::nullopt;

    const ForestPlacementIndirectDraw initialDraw{.IndexCount = indexCount};
    std::memcpy(pageUpload.Ptr, pages.data(), pages.size_bytes());
    std::memcpy(viewUpload.Ptr, &view, sizeof(view));
    std::memcpy(argsUpload.Ptr, &initialDraw, sizeof(initialDraw));
    if (useCoarse)
    {
        const ForestPlacementIndirectDraw coarseDraw{.IndexCount = coarseIndexCount};
        std::memcpy(static_cast<std::byte*>(argsUpload.Ptr) + sizeof(initialDraw), &coarseDraw, sizeof(coarseDraw));
    }

    const auto slotBuffer = frame.ImportExternalBuffer("Forest.PlacementSlots", slots, slotBytes);
    const auto pageBuffer = frame.ImportExternalBuffer("Forest.CullPages", pageUpload.Buffer);
    const auto viewBuffer = frame.ImportExternalBuffer("Forest.CullView", viewUpload.Buffer);
    const auto argsStaging = frame.ImportExternalBuffer("Forest.CullArgsUpload", argsUpload.Buffer);
    RGBuffer windBuffer{};
    uint64 windOffset = 0;
    if (wind)
    {
        const auto windUpload = frame.AllocUpload(sizeof(*wind));
        if (!windUpload.Valid())
            return std::nullopt;
        auto preparedWind = *wind;
        if (!useCoarse)
            preparedWind.Lod = {};
        std::memcpy(windUpload.Ptr, &preparedWind, sizeof(preparedWind));
        windBuffer = frame.ImportExternalBuffer("Forest.SplatWind", windUpload.Buffer);
        windOffset = windUpload.Offset;
        if (!windBuffer.IsValid())
            return std::nullopt;
    }

    const BufferDesc visibleDesc{.size = visibleBytes,
        .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferSrc),
        .memoryUsage = BufferMemoryUsage::DeviceLocal, .stride = visibleStride};
    const BufferDesc argsDesc{.size = sizeof(initialDraw),
        .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferDst |
                                     BufferUsage::TransferSrc),
        .memoryUsage = BufferMemoryUsage::DeviceLocal, .stride = sizeof(uint32)};
    // Forward contributors bind physical buffers during graph declaration.
    const auto visible = frame.ImportPersistentBuffer((std::string(scope) + ".VisiblePlacements").c_str(), visibleDesc);
    const auto args = frame.ImportPersistentBuffer((std::string(scope) + ".IndirectDraw").c_str(), argsDesc);
    if (!visible.IsValid() || !args.IsValid() || !frame.PhysicalBuffer(visible).IsValid() ||
        !frame.PhysicalBuffer(args).IsValid())
        return std::nullopt;
    RGBuffer coarseVisible{};
    RGBuffer coarseArgs{};
    if (useCoarse)
    {
        coarseVisible = frame.ImportPersistentBuffer((std::string(scope) + ".CoarseVisiblePlacements").c_str(), visibleDesc);
        coarseArgs = frame.ImportPersistentBuffer((std::string(scope) + ".CoarseIndirectDraw").c_str(), argsDesc);
        if (!coarseVisible.IsValid() || !coarseArgs.IsValid() || !frame.PhysicalBuffer(coarseVisible).IsValid() ||
            !frame.PhysicalBuffer(coarseArgs).IsValid())
            return std::nullopt;
    }
    if (m_VisiblePageCapacity != visiblePages)
    {
        m_VisiblePageCapacity = visiblePages;
        static const bool logCapacity = Platform::EnvironmentSwitchEnabled("GE_FOREST_STATS", false);
        if (logCapacity)
            LOG_INFO("Forest: visible placement buffer '{}' reserves {} pages ({} submitted, {} slots, {} bytes)",
                     scope, visiblePages, pages.size(), slotCount, visibleBytes);
    }

    const auto reset = frame.AddPass("Forest.ResetIndirectDraw", PassPhase::kEarlySetup,
        [=](RGPassBuilder& pass)
        {
            pass.Read(argsStaging, RGBufferRead::CopySrc);
            pass.Write(args, RGBufferWrite::CopyDst);
            if (useCoarse)
                pass.Write(coarseArgs, RGBufferWrite::CopyDst);
        },
        [=, offset = argsUpload.Offset](RGContext& ctx)
        {
            if (!ctx.Cmd)
                return;
            ctx.Cmd->CopyBuffer(ctx.GetBuffer(argsStaging), ctx.GetBuffer(args), sizeof(initialDraw), offset, 0);
            if (useCoarse)
                ctx.Cmd->CopyBuffer(ctx.GetBuffer(argsStaging), ctx.GetBuffer(coarseArgs), sizeof(initialDraw),
                                    offset + sizeof(initialDraw), 0);
        });
    if (!reset.IsValid())
        return std::nullopt;

    const auto pass = frame.AddPass(useOcclusion ? "Forest.CullOccludedPlacements" : "Forest.CullPlacements",
        useOcclusion ? PassPhase::kDefault : PassPhase::kEarlySetup,
        [=, this](RGPassBuilder& builder)
        {
            builder.UsePipeline(cullPipeline);
            builder.Read(slotBuffer, RGBufferRead::Storage);
            builder.Read(pageBuffer, RGBufferRead::Storage);
            builder.Read(viewBuffer, RGBufferRead::Uniform);
            if (windBuffer.IsValid())
                builder.Read(windBuffer, RGBufferRead::Uniform);
            if (useOcclusion)
                builder.Read(occlusionHZB, RGTextureRead::Sampled);
            builder.Read(args, RGBufferRead::Storage);
            builder.Write(visible, RGBufferWrite::Storage);
            builder.Write(args, RGBufferWrite::Storage);
            if (useCoarse)
            {
                builder.Read(coarseArgs, RGBufferRead::Storage);
                builder.Write(coarseVisible, RGBufferWrite::Storage);
                builder.Write(coarseArgs, RGBufferWrite::Storage);
            }
        },
        [=, this, groups = static_cast<uint32>(pages.size()), pageBytes = pages.size_bytes(),
         pageOffset = pageUpload.Offset, viewOffset = viewUpload.Offset](RGContext& ctx)
        {
            const auto pipeline = ctx.GetOrCreatePipelineVariant(cullPipeline);
            auto* passDevice = ctx.GetDevice();
            if (!pipeline.IsValid() || !passDevice || !ctx.Cmd)
                return;

            std::array<DescriptorWrite, 6> placements{{
                {.Binding = kSlotsBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(slotBuffer), .Range = slotBytes},
                {.Binding = kPagesBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(pageBuffer), .Offset = pageOffset, .Range = pageBytes},
                {.Binding = kVisibleBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(visible), .Range = visibleBytes},
                {.Binding = kArgsBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(args), .Range = sizeof(initialDraw)}}};
            if (m_Splats)
            {
                placements[4] = {.Binding = kCoarseVisibleBinding, .Type = DescriptorType::StorageBuffer,
                    .Buffer = ctx.GetBuffer(useCoarse ? coarseVisible : visible), .Range = visibleBytes};
                placements[5] = {.Binding = kCoarseArgsBinding, .Type = DescriptorType::StorageBuffer,
                    .Buffer = ctx.GetBuffer(useCoarse ? coarseArgs : args), .Range = sizeof(initialDraw)};
            }
            std::array<DescriptorWrite, 2> camera{{
                {.Binding = kViewBinding, .Type = DescriptorType::UniformBuffer,
                 .Buffer = ctx.GetBuffer(viewBuffer), .Offset = viewOffset, .Range = sizeof(ForestPlacementGpuView)}}};
            if (windBuffer.IsValid())
                camera[1] = {.Binding = kWindBinding, .Type = DescriptorType::UniformBuffer,
                             .Buffer = ctx.GetBuffer(windBuffer), .Offset = windOffset, .Range = sizeof(ForestSplatWindGpu)};
            const auto set0 = passDevice->GetOrCreateDescriptorSet(
                m_PlacementsLayout, std::span(placements).first(m_Splats ? 6u : 4u), "Forest.Cull.Placements");
            const auto set1 = passDevice->GetOrCreateDescriptorSet(
                m_ViewLayout, std::span(camera).first(windBuffer.IsValid() ? 2u : 1u), "Forest.Cull.View");
            if (!set0.IsValid() || !set1.IsValid())
                return;
            DescriptorSetHandle occlusionSet;
            if (useOcclusion)
            {
                const DescriptorWrite depth{.Binding = kOcclusionBinding, .Type = DescriptorType::CombinedImageSampler,
                    .Texture = ctx.GetTexture(occlusionHZB), .Sampler = occlusionSampler};
                occlusionSet = passDevice->GetOrCreateDescriptorSet(m_OcclusionLayout, std::span(&depth, 1),
                                                                     "Forest.Cull.Occlusion");
                if (!occlusionSet.IsValid())
                    return;
            }
            ctx.Cmd->SetPipeline(pipeline);
            ctx.Cmd->BindDescriptorSet(0, set0, pipeline);
            ctx.Cmd->BindDescriptorSet(1, set1, pipeline);
            if (useOcclusion)
                ctx.Cmd->BindDescriptorSet(2, occlusionSet, pipeline);
            ctx.Cmd->Dispatch(groups, 1, 1);
        });
    if (!pass.IsValid())
        return std::nullopt;

    return DrawBuffers{slotBuffer, visible, args, pageBuffer, viewBuffer, slotBytes, visibleBytes,
                       pageUpload.Offset, pages.size_bytes(), viewUpload.Offset, coarseVisible, coarseArgs};
}

} // namespace GameEngine::Engine::Renderer
