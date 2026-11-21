#include "Engine/Rendering/Forest/ForestPlacementCull.h"

#include "Engine/Rendering/Forest/ForestPlacementGpuData.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
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
constexpr uint32 kViewBinding = 0;
constexpr uint32 kWindBinding = 1;
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
    return true;
}

std::optional<ForestPlacementCull::DrawBuffers> ForestPlacementCull::Declare(RGFrame& frame,
    std::string_view scope, BufferHandle slots, uint32 slotCount, const ForestPlacementGpuView& view,
    std::span<const ForestPlacementGpuPage> pages, uint32 indexCount, const ForestSplatWindGpu* wind)
{
    if (!m_Pipeline.IsValid() || !slots.IsValid() || pages.empty() || pages.size() > slotCount ||
        pages.size() > kMaxCullGroups)
        return std::nullopt;
    auto* device = frame.Device();
    if (!device || device->RequestComputePipeline(m_Pipeline) != PipelineBuildState::Warm)
        return std::nullopt;

    const uint64 slotBytes = uint64{slotCount} * kSlotSize * sizeof(PageStreaming::PackedPlacement);
    const uint32 visibleStride = wind ? sizeof(ForestVisibleSplatTree) : sizeof(ForestPlacementGpuVisible);
    const uint32 visiblePages = std::min(slotCount, std::max(m_VisiblePageCapacity,
        std::bit_ceil(static_cast<uint32>(pages.size()))));
    const uint64 visibleBytes = uint64{visiblePages} * kSlotSize * visibleStride;
    const auto pageUpload = frame.AllocUpload(pages.size_bytes());
    const auto viewUpload = frame.AllocUpload(sizeof(view));
    const auto argsUpload = frame.AllocUpload(sizeof(ForestPlacementIndirectDraw));
    if (!pageUpload.Valid() || !viewUpload.Valid() || !argsUpload.Valid())
        return std::nullopt;

    const ForestPlacementIndirectDraw initialDraw{.IndexCount = indexCount};
    std::memcpy(pageUpload.Ptr, pages.data(), pages.size_bytes());
    std::memcpy(viewUpload.Ptr, &view, sizeof(view));
    std::memcpy(argsUpload.Ptr, &initialDraw, sizeof(initialDraw));

    const auto slotBuffer = frame.ImportExternalBuffer("Forest.PlacementSlots", slots, slotBytes);
    const auto pageBuffer = frame.ImportExternalBuffer("Forest.CullPages", pageUpload.Buffer);
    const auto viewBuffer = frame.ImportExternalBuffer("Forest.CullView", viewUpload.Buffer);
    const auto argsStaging = frame.ImportExternalBuffer("Forest.CullArgsUpload", argsUpload.Buffer);
    RGBuffer windBuffer{};
    uint64 windOffset = 0;
    if (wind)
    {
        const auto upload = frame.AllocUpload(sizeof(*wind));
        if (!upload.Valid())
            return std::nullopt;
        std::memcpy(upload.Ptr, wind, sizeof(*wind));
        windBuffer = frame.ImportExternalBuffer("Forest.SplatWind", upload.Buffer);
        windOffset = upload.Offset;
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
        },
        [=, offset = argsUpload.Offset](RGContext& ctx)
        {
            if (ctx.Cmd)
                ctx.Cmd->CopyBuffer(ctx.GetBuffer(argsStaging), ctx.GetBuffer(args), sizeof(initialDraw), offset, 0);
        });
    if (!reset.IsValid())
        return std::nullopt;

    const auto pass = frame.AddPass("Forest.CullPlacements", PassPhase::kEarlySetup,
        [=, this](RGPassBuilder& builder)
        {
            builder.UsePipeline(m_Pipeline);
            builder.Read(slotBuffer, RGBufferRead::Storage);
            builder.Read(pageBuffer, RGBufferRead::Storage);
            builder.Read(viewBuffer, RGBufferRead::Uniform);
            if (windBuffer.IsValid())
                builder.Read(windBuffer, RGBufferRead::Uniform);
            builder.Read(args, RGBufferRead::Storage);
            builder.Write(visible, RGBufferWrite::Storage);
            builder.Write(args, RGBufferWrite::Storage);
        },
        [=, this, groups = static_cast<uint32>(pages.size()), pageBytes = pages.size_bytes(),
         pageOffset = pageUpload.Offset, viewOffset = viewUpload.Offset](RGContext& ctx)
        {
            const auto pipeline = ctx.GetOrCreatePipelineVariant(m_Pipeline);
            auto* passDevice = ctx.GetDevice();
            if (!pipeline.IsValid() || !passDevice || !ctx.Cmd)
                return;

            const DescriptorWrite placements[] = {
                {.Binding = kSlotsBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(slotBuffer), .Range = slotBytes},
                {.Binding = kPagesBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(pageBuffer), .Offset = pageOffset, .Range = pageBytes},
                {.Binding = kVisibleBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(visible), .Range = visibleBytes},
                {.Binding = kArgsBinding, .Type = DescriptorType::StorageBuffer,
                 .Buffer = ctx.GetBuffer(args), .Range = sizeof(initialDraw)}};
            std::array<DescriptorWrite, 2> camera{{
                {.Binding = kViewBinding, .Type = DescriptorType::UniformBuffer,
                 .Buffer = ctx.GetBuffer(viewBuffer), .Offset = viewOffset, .Range = sizeof(ForestPlacementGpuView)}}};
            if (windBuffer.IsValid())
                camera[1] = {.Binding = kWindBinding, .Type = DescriptorType::UniformBuffer,
                             .Buffer = ctx.GetBuffer(windBuffer), .Offset = windOffset, .Range = sizeof(ForestSplatWindGpu)};
            const auto set0 = passDevice->GetOrCreateDescriptorSet(m_PlacementsLayout, placements, "Forest.Cull.Placements");
            const auto set1 = passDevice->GetOrCreateDescriptorSet(m_ViewLayout, std::span(camera).first(windBuffer.IsValid() ? 2u : 1u),
                                                                        "Forest.Cull.View");
            if (!set0.IsValid() || !set1.IsValid())
                return;
            ctx.Cmd->SetPipeline(pipeline);
            ctx.Cmd->BindDescriptorSet(0, set0, pipeline);
            ctx.Cmd->BindDescriptorSet(1, set1, pipeline);
            ctx.Cmd->Dispatch(groups, 1, 1);
        });
    if (!pass.IsValid())
        return std::nullopt;

    return DrawBuffers{slotBuffer, visible, args, pageBuffer, viewBuffer, slotBytes, visibleBytes,
                       pageUpload.Offset, pages.size_bytes(), viewUpload.Offset};
}

} // namespace GameEngine::Engine::Renderer
