#include "Engine/Rendering/VirtualShadows/VirtualShadowRenderFeature.h"

#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/FeatureDeclareContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowFeedback.h"
#include "Rendering/Core/ReverseZ.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace RenderGraph = Rendering::RenderGraph;
struct VirtualShadowRenderFeature::PreparedPage
{
    VirtualShadowPageWork Work;
    uint32_t TableIndex = 0;
    PreparedDepthOnlyPass* Prepared = nullptr;
    DepthProducerState Producer;
    bool Render = false;
    bool Recorded = false;
};

namespace
{
constexpr uint32_t kPageDrawBudget = 24;

std::optional<Rendering::CameraData> PageCamera(const VirtualShadowFeedbackProjection& projection,
    const VirtualShadowPage& page, const Rendering::CameraData& camera)
{
    Rendering::CameraData result = camera;
    if (projection.DirectionalGrid)
    {
        const Mathematics::Vector3Int sector{camera.renderOriginSector[0], camera.renderOriginSector[1],
            camera.renderOriginSector[2]};
        result.viewProj = VirtualShadowDirectionalPageMatrix(*projection.DirectionalGrid, page,
                                                            Mathematics::Vector3Int{});
        result.viewProjRel = VirtualShadowDirectionalPageMatrix(*projection.DirectionalGrid, page, sector);
        return result;
    }
    const uint32_t side = projection.PagesPerAxis >> page.Mip;
    const auto x = static_cast<uint32_t>(page.X - projection.BasePage.X);
    const auto y = static_cast<uint32_t>(page.Y - projection.BasePage.Y);
    const auto projected = CropVirtualShadowProjection(camera.proj, side, x, y);
    const auto world = CropVirtualShadowProjection(camera.viewProj, side, x, y);
    const auto relative = CropVirtualShadowProjection(camera.viewProjRel, side, x, y);
    if (!projected || !world || !relative)
        return std::nullopt;
    result.proj = *projected;
    result.viewProj = *world;
    result.viewProjRel = *relative;
    return result;
}

DepthPassType PagePassType(VirtualShadowLight family)
{
    switch (family)
    {
    case VirtualShadowLight::Directional: return DepthPassType::ShadowCascade;
    case VirtualShadowLight::Spot: return DepthPassType::SpotShadow;
    case VirtualShadowLight::Point: return DepthPassType::PointShadow;
    }
    return DepthPassType::Count;
}
} // namespace

bool VirtualShadowRenderFeature::PreparePageTable(RenderGraph::RGPrepareContext& context,
    const RasterPlan& plan, Rendering::ViewId view, std::span<PreparedPage> pages,
    std::span<uint32_t> words, RenderServices& services)
{
    std::fill(words.begin(), words.end(), kUnmappedShadowPage);
    for (auto& page : pages)
    {
        page.Recorded = false;
        page.Producer = {};
    }
    auto depth = context;
    if (!depth.IsFor(plan.For) || !depth.UseDepthTarget(plan.Atlas, 1u))
        return false;
    const auto inputs = services.MakeDepthDrawServices();
    for (auto& page : pages)
    {
        if (page.Render && !IsCurrentPage(view, plan, page.Work))
            return false;
        if (!PrepareDepthOnlyPass(depth, *page.Prepared, inputs))
            return false;
        page.Producer = GetPreparedDepthProducerState(*page.Prepared, CascadeCasters::All);
        if (!page.Render && ValidatePage(view, plan, page.Work, page.Producer))
            words[page.TableIndex] = page.Work.Tile;
    }
    return true;
}

RenderGraph::RGBuffer VirtualShadowRenderFeature::DeclarePages(RenderGraph::RGFrame& frame,
    RenderServices& services, const FeatureDeclareContext& context, const RasterPlan& plan,
    const VirtualShadowFeedbackLayout& layout, std::span<const Rendering::CameraData> cameras)
{
    if (!plan.For.IsFor(frame) || !context.For.IsFor(frame) || context.WorldAlreadyDeclared ||
        !plan.Atlas.IsValid() || !context.DrawStreamOrdering.IsValid() ||
        (context.HasPendingSkinnedInstances && !context.SkinPaletteAtlas.IsValid()) ||
        cameras.size() != layout.Projections.size() || !MatchesLayout(context.View, plan, layout) ||
        layout.WordCount == 0)
        return {};

    const size_t entryCount = static_cast<size_t>(layout.WordCount) * 32;
    const size_t bytes = entryCount * sizeof(uint32_t);
    const auto staging = frame.AllocUpload(bytes);
    if (!staging.Valid())
        return {};
    auto* words = static_cast<uint32_t*>(staging.Ptr);
    std::fill_n(words, entryCount, kUnmappedShadowPage);

    auto* pages = frame.AllocatePassData<std::vector<PreparedPage>>();
    pages->reserve(plan.Work.size());
    uint32_t renderCount = 0;
    for (const auto& work : plan.Work)
    {
        if (work.Tile == kUnmappedShadowPage || (work.NeedsRender && renderCount == kPageDrawBudget))
            continue;
        size_t index = 0;
        std::optional<uint32_t> bit;
        for (; index < layout.Projections.size(); ++index)
        {
            bit = FindVirtualShadowFeedbackBit(layout.Projections[index], work.Page);
            if (bit)
                break;
        }
        if (!bit || *bit >= entryCount)
            continue;
        const auto passType = PagePassType(work.Page.Family);
        if (passType == DepthPassType::Count ||
            !ContributorDepthCommands(services.Views(), context.View, passType,
                                     Rendering::GPUDrawStreamBuilder::SlicePhase::A).empty())
            continue;
        const auto pageCamera = PageCamera(layout.Projections[index], work.Page, cameras[index]);
        if (!pageCamera)
            continue;
        const auto camera = frame.AllocUpload<Rendering::CameraData>();
        if (!camera.Valid())
            return {};
        *camera.Ptr = *pageCamera;

        DepthOnlyPassParamsRG parameters{};
        parameters.ViewId = context.View;
        parameters.PassType = passType;
        parameters.CascadeIndex = static_cast<uint32_t>(index);
        parameters.VirtualPageTile = work.Tile;
        parameters.VirtualFrame = context.For;
        parameters.VirtualCasterOrdering = context.DrawStreamOrdering;
        parameters.Casters = CascadeCasters::All;
        parameters.Phase = Rendering::GPUDrawStreamBuilder::SlicePhase::A;
        parameters.DepthBiasEnable = true;
        parameters.DepthClampEnable = work.Page.Family == VirtualShadowLight::Directional;
        parameters.RasterizationSamples = 1;
        parameters.ViewportWidth = parameters.ViewportHeight = kVirtualShadowPageTexels;
        parameters.Resources = services.BuildShadowPassResources(context.Seam, frame, context.View,
                                                                  camera.Buffer, camera.Offset);
        auto* prepared = AllocatePreparedDepthOnlyPass(frame, std::move(parameters));
        pages->push_back({work, *bit, prepared, {}, work.NeedsRender, false});
        if (work.NeedsRender)
            ++renderCount;
    }

    Rendering::BufferDesc descriptor{};
    descriptor.size = bytes;
    descriptor.stride = sizeof(uint32_t);
    descriptor.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage | Rendering::BufferUsage::TransferDst);
    descriptor.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    const std::string name = "VirtualShadowPageTable.View" + std::to_string(context.View);
    descriptor.debugName = name.c_str();
    // A lighting pass binds the table when it is declared, so the table needs a physical buffer then.
    // The upload pass writes all rows in each frame, so the old content is not read.
    const auto table = frame.ImportPersistentBuffer(name.c_str(), descriptor);
    if (!table.IsValid())
        return {};
    frame.MarkOutput(table);

    auto* servicesPointer = &services;
    const auto view = context.View;
    const auto ordering = context.DrawStreamOrdering;
    const auto skin = context.SkinPaletteAtlas;
    auto* ready = frame.AllocatePassData<bool>(false);
    const std::string uploadName = "UploadVirtualShadowPageTable.View" + std::to_string(view);
    frame.AddPass(uploadName.c_str(), Rendering::PassPhase::kEarlySetup,
        [this, pages, words, entryCount, table, plan, view, ordering, skin, servicesPointer, ready]
        (RenderGraph::RGPassBuilder& builder)
        {
            builder.Read(ordering, RenderGraph::RGBufferRead::Indirect);
            builder.Read(plan.Atlas);
            if (skin.IsValid())
                builder.Read(skin);
            builder.Write(table, RenderGraph::RGBufferWrite::CopyDst);
            builder.Prepare([this, pages, words, entryCount, plan, view, servicesPointer, ready]
            (RenderGraph::RGPrepareContext& preparation)
            {
                *ready = false;
                *ready = PreparePageTable(preparation, plan, view, *pages,
                                         std::span<uint32_t>(words, entryCount), *servicesPointer);
                return *ready;
            });
        },
        [table, staging, bytes](RenderGraph::RGContext& recording)
        {
            const auto destination = recording.GetBuffer(table);
            if (recording.Cmd && destination.IsValid())
                recording.Cmd->CopyBuffer(staging.Buffer, destination, bytes, staging.Offset, 0);
        });

    for (auto& page : *pages)
    {
        if (!page.Render)
            continue;
        auto* packet = &page;
        const std::string passName = "VirtualShadowDepth.View" + std::to_string(view) +
                                     ".Tile" + std::to_string(page.Work.Tile);
        frame.AddPass(passName.c_str(), Rendering::PassPhase::kEarlySetup,
            [this, packet, plan, view, ordering, skin, ready](RenderGraph::RGPassBuilder& builder)
            {
                builder.Read(ordering, RenderGraph::RGBufferRead::Indirect);
                if (skin.IsValid())
                    builder.Read(skin);
                builder.RecordInSecondary();
                RenderGraph::RGAttachmentOps operations{};
                operations.Load = RenderGraph::RGLoadOp::Clear;
                operations.Store = RenderGraph::RGStoreOp::Store;
                operations.Clear.Depth = Rendering::ReverseZ::kClearFar;
                builder.AttachDepth(plan.Atlas, operations, RenderGraph::RGDepthAccess::ReadWrite,
                    RenderGraph::RGRange{.BaseLayer = packet->Work.Tile, .LayerCount = 1});
                builder.OnSubmitted([this, packet, plan, view, ready]
                {
                    CertifyPage(view, plan, packet->Work, packet->Producer, packet->Recorded && *ready);
                });
            },
            [packet, ready](RenderGraph::RGContext& recording)
            {
                if (!*ready || !recording.Cmd || !packet->Producer.Complete)
                    return;
                RecordPreparedDepthOnlyPass(recording, *packet->Prepared);
                packet->Recorded = true;
            });
    }
    return table;
}
} // namespace GameEngine::Engine::Renderer
