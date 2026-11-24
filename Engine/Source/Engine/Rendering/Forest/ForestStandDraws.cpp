#include "ForestStandDraws.h"

#include "../IdentityMeshDrawConstants.h"
#include "ForestStandDrawProducer.h"

#include "ForestPlacementFeature.h"
#include "ForestPlacementSession.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "ForestPlacementGpuData.h"
#include "ForestPlacementRuntime.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

namespace GameEngine::Engine::Renderer
{
namespace
{

bool ApplyStandIndexRange(const DrawCommand& head, std::span<DrawSubCommand> ranges)
{
    // The head supplies the mesh's LOD0 allocation in the shared index and vertex buffers.
    const uint64 lastEnd = uint64{ranges.back().FirstIndex} + ranges.back().IndexCount;
    if ((head.IndexCount != 0 && lastEnd > head.IndexCount) ||
        uint64{head.FirstIndex} + lastEnd > std::numeric_limits<uint32>::max())
        return false;

    for (auto& range : ranges)
    {
        range.FirstIndex += head.FirstIndex;
        range.VertexOffset = head.VertexOffset;
    }
    return true;
}

} // namespace

ForestPlacementView MakeForestStandView(const Rendering::CameraData& camera)
{
    ForestPlacementView view{CameraEye(camera), {}};
    Rendering::ExtractFrustumPlanes(camera.viewProjRel, view.EyeRelativePlanes.data());
    const Mathematics::Vector3 eye = CameraEyeRelativeToOrigin(camera);
    for (auto& plane : view.EyeRelativePlanes)
        plane.w += plane.x * eye.x + plane.y * eye.y + plane.z * eye.z;
    return view;
}

const Rendering::CameraData* FindForestStandCamera(RenderServices& services, Rendering::ViewId viewId,
                                                   uint64 worldId, Rendering::RenderLayerMask layers)
{
    auto& views = services.Views();
    const auto* view = views.FindViewDesc(viewId);
    if (!view || view->worldId != worldId || !layers.Intersects(view->ActiveRenderLayerMask()))
        return nullptr;
    return views.FindCameraData(view->cameraId);
}

namespace
{
struct StandSectorRange
{
    uint32 IndexEnd;
    Mathematics::Vector3Int Sector;
};

// What one registered stand draws, shared by its forward-emit callback.
struct StandEmitSource
{
    uint64 WorldId;
    Rendering::RenderLayerMask Layers;
    std::shared_ptr<ForestPlacementRuntime> Placements;
    PrepareForestStandDraw PrepareDraw;
    std::shared_ptr<ForestPlacementSession> Details;
    std::vector<StandSectorRange> Sectors;
};

Rendering::DrawProducerId StandProducer()
{
    static const auto producer = Rendering::DrawProducerRegistry::Register("ForestStand");
    return producer;
}

// The camera of a view of the stand's world that shows one of its layers.
const Rendering::CameraData* FindStandCamera(const ForwardEmitContext& ctx, const StandEmitSource& stand)
{
    if (!ctx.Frame)
        return nullptr;
    return FindForestStandCamera(*ctx.Services, ctx.ViewId, stand.WorldId, stand.Layers);
}

// True when the heads draw directly and a depth head addresses the same indices as the color head.
bool AreDrawableHeads(const ForestStandDrawHeads& heads)
{
    if (heads.Color.UseIndirect)
        return false;
    if (!heads.Depth)
        return true;
    return !heads.Depth->UseIndirect && heads.Depth->FirstIndex == heads.Color.FirstIndex &&
           heads.Depth->VertexOffset == heads.Color.VertexOffset && heads.Depth->IndexCount == heads.Color.IndexCount;
}

void EmitStandCommands(ForwardEmitContext& ctx, ForestStandDrawHeads& heads, std::span<const DrawSubCommand> draws)
{
    heads.Color.Producer = StandProducer();
    heads.Color.SubDraws = draws;
    if (!heads.Depth || ctx.Purpose != ForwardEmitPurpose::World)
    {
        ctx.Services->EmitForwardCommand(ctx.ViewId, heads.Color, ForwardDrawDepth::ColourPass);
        return;
    }
    heads.Depth->Producer = StandProducer();
    heads.Depth->SubDraws = draws;
    ctx.Services->EmitForwardCommand(ctx.ViewId, heads.Color, ForwardDrawDepth::Prepass, &*heads.Depth);
}

// True when the color head's material can draw with the LOD crossfade in this view.
bool WarmStandFadeVariant(const ForwardEmitContext& ctx, const DrawCommand& color)
{
    auto& views = ctx.Services->Views();
    const auto pass = views.GetViewDeclaredWorldPassKeywords(ctx.ViewId);
    const auto format = views.GetViewWorldColorFormatKey(ctx.ViewId);
    return pass && format.ColorCount != 0 && color.Material &&
           ctx.Services->Materials().Variants().WarmColorVariant(*color.Material, color.VertexFlags,
               Rendering::PrimitiveTopology::TriangleList,
               (*pass & ~Rendering::MaterialKeyword::Instanced) | Rendering::MaterialKeyword::LodCrossfade,
               ctx.Services->GetFrontFace(), format);
}

// Split asset-relative ranges at sector boundaries. Each head retains the holes between visible runs.
template<typename Emit>
void EmitSectorRanges(StandEmitSource& stand, Rendering::RenderGraph::RGFrame& frame, ForestStandDrawHeads& heads,
                      std::span<const DrawSubCommand> draws, Emit emit, uint32 fadeCode = 0)
{
    constexpr uint32 kIndicesPerQuad = 6;
    if (stand.Sectors.empty())
    {
        uint32 indexEnd = 0;
        for (const auto& page : stand.Placements->Store().Pages())
        {
            const auto sector = page.OverviewSector();
            indexEnd += kIndicesPerQuad;
            if (stand.Sectors.empty() || stand.Sectors.back().Sector != sector)
                stand.Sectors.push_back({indexEnd, sector});
            else
                stand.Sectors.back().IndexEnd = indexEnd;
        }
    }

    const uint32 firstIndex = heads.Color.FirstIndex;
    std::vector<DrawSubCommand>* group = nullptr;
    const StandSectorRange* current = nullptr;
    for (const auto& draw : draws)
    {
        uint32 begin = draw.FirstIndex - firstIndex;
        const uint32 end = begin + draw.IndexCount;
        while (begin < end)
        {
            if (!current || begin >= current->IndexEnd)
            {
                if (group)
                    emit(heads, *group);
                current = &*std::ranges::upper_bound(stand.Sectors, begin, {}, &StandSectorRange::IndexEnd);
                group = frame.AllocatePassData<std::vector<DrawSubCommand>>();
                auto* constants = frame.AllocatePassData<IdentityMeshDrawConstants>(*heads.SectorLocalMaterialIndex);
                constants->NormalColumn0.w = std::bit_cast<float>(current->Sector.X);
                constants->NormalColumn1.w = std::bit_cast<float>(current->Sector.Y);
                constants->NormalColumn2.w = std::bit_cast<float>(current->Sector.Z);
                constants->SectorAndFade = 1u | fadeCode;
                heads.Color.Bindings.PushConstants = std::as_bytes(std::span(constants, 1u));
                if (heads.Depth)
                    heads.Depth->Bindings.PushConstants = heads.Color.Bindings.PushConstants;
            }
            const uint32 stop = std::min(end, current->IndexEnd);
            auto part = draw;
            part.FirstIndex = firstIndex + begin;
            part.IndexCount = stop - begin;
            group->push_back(part);
            begin = stop;
        }
    }
    if (group)
        emit(heads, *group);
}

// Draws the overview quads of the handoff band on the pixels that their trees leave. While the crossfade
// variant warms, the quads join the full overview draws under their trees.
void EmitStandFades(StandEmitSource& stand, ForwardEmitContext& ctx, const ForestStandDrawHeads& heads,
                    std::span<const ForestStandFade> fades, std::vector<DrawSubCommand>& draws)
{
    if (fades.empty())
        return;
    if ((!heads.SectorLocalMaterialIndex && heads.Color.Bindings.PushConstants.size() != sizeof(IdentityMeshDrawConstants)) ||
        !WarmStandFadeVariant(ctx, heads.Color))
    {
        for (const ForestStandFade& fade : fades)
            draws.push_back(fade.Range);
        std::ranges::sort(draws, {}, &DrawSubCommand::FirstIndex);
        return;
    }
    using FadeEncoding = Rendering::GPUDrawStreamBuilder;
    struct FadeRanges
    {
        uint32 Code = 0;
        Vector<DrawSubCommand> Ranges;
    };
    std::array<FadeRanges, FadeEncoding::kLodFadeMaxWeight + 1u> batches;
    for (const ForestStandFade& fade : fades)
    {
        auto range = fade.Range;
        if (!ApplyStandIndexRange(heads.Color, std::span(&range, 1)))
            continue;
        const uint32 code = ForestHandoffFadeCode(fade.DetailWeight, true);
        const uint32 bin = (code >> FadeEncoding::kLodFadeWeightShift) & FadeEncoding::kLodFadeMaxWeight;
        auto& batch = batches[bin];
        batch.Code = code;
        batch.Ranges.push_back(range);
    }

    // The fade has one packed value per draw. Disjoint overview quads with the
    // same value can use one draw head and push-constant allocation.
    for (auto& batch : batches)
    {
        if (batch.Ranges.empty())
            continue;
        auto* ranges = ctx.Frame->AllocatePassData<Vector<DrawSubCommand>>(std::move(batch.Ranges));
        ForestStandDrawHeads fading{};
        fading.Color = heads.Color;
        fading.Color.PassKeywords |= Rendering::MaterialKeyword::LodCrossfade;
        fading.SectorLocalMaterialIndex = heads.SectorLocalMaterialIndex;
        if (fading.SectorLocalMaterialIndex)
        {
            EmitSectorRanges(stand, *ctx.Frame, fading, *ranges,
                [&ctx](ForestStandDrawHeads& group, std::span<const DrawSubCommand> subDraws)
                { EmitStandCommands(ctx, group, subDraws); }, batch.Code);
            continue;
        }
        auto* constants = ctx.Frame->AllocatePassData<IdentityMeshDrawConstants>(0u);
        std::memcpy(constants, fading.Color.Bindings.PushConstants.data(), sizeof(*constants));
        constants->SectorAndFade = batch.Code;
        fading.Color.Bindings.PushConstants = std::as_bytes(std::span(constants, 1));
        EmitStandCommands(ctx, fading, *ranges);
    }
}

void EmitStand(StandEmitSource& stand, ForwardEmitContext& ctx)
{
    const auto* camera = FindStandCamera(ctx, stand);
    if (!camera || (stand.Details && !stand.Details->DrawsColor))
        return;

    auto* draws = ctx.Frame->AllocatePassData<std::vector<DrawSubCommand>>();
    const auto placementView = MakeForestStandView(*camera);
    const bool usesDetails = stand.Details && ctx.Purpose == ForwardEmitPurpose::World;
    if (!usesDetails)
    {
        stand.Placements->GatherStand(placementView, *draws);
        if (draws->empty())
            return;
    }
    auto heads = stand.PrepareDraw(ctx);
    if (!heads || !AreDrawableHeads(*heads))
        return;

    if (usesDetails)
    {
        std::vector<ForestStandFade> fades;
        auto* detailFeature = ctx.Services->GetFeature<ForestPlacementFeature>();
        if (!detailFeature || !ctx.Frame->RequireExecutionLifetime(stand.Details) ||
            !detailFeature->EmitDetails(*stand.Details, ctx, *camera, placementView, *draws, fades))
            stand.Placements->GatherStand(placementView, *draws);
        else
            EmitStandFades(stand, ctx, *heads, fades, *draws);
    }
    if (draws->empty() || !ApplyStandIndexRange(heads->Color, *draws))
        return;
    if (heads->SectorLocalMaterialIndex)
        EmitSectorRanges(stand, *ctx.Frame, *heads, *draws,
            [&ctx](ForestStandDrawHeads& group, std::span<const DrawSubCommand> ranges)
            { EmitStandCommands(ctx, group, ranges); });
    else
        EmitStandCommands(ctx, *heads, *draws);
}

// The shaded head of the overview mesh, or none while the mesh or its material is not drawable.
std::optional<ForestStandDrawHeads> PrepareStandMesh(RenderServices& services, Rendering::RenderGraph::RGFrame& frame,
                                                     const Rendering::MeshGPUKey& mesh,
                                                     uint32 indexEnd, const GUID& material,
                                                     const ForwardEmitContext* forward = nullptr)
{
    if (material.IsNull())
        return std::nullopt;
    auto& registry = services.GetMeshGPURegistry();
    const Rendering::MeshGPURegistry::TableScope scope(registry);
    const auto* geometry = registry.FindByKey(mesh);
    const auto* mat = services.Materials().Registry().Find(material);
    Rendering::MeshGPUEntryBindings bindings{};
    if (!mat || !geometry || geometry->vertexFlags != Rendering::VertexAttributeFlags::StandardMesh ||
        geometry->topology != Rendering::PrimitiveTopology::TriangleList ||
        geometry->indexCount != indexEnd ||
        !mat->GetCompileSpec().vertexModifierPath.empty() || mat->GetCompileSpec().customVertexShader ||
        (mat->GetAlphaMode() != MaterialAlphaMode::Mask && mat->GetAlphaMode() != MaterialAlphaMode::Opaque) ||
        geometry->vertexOffset > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
        !registry.TryGetDrawableBindings(*geometry, bindings))
        return std::nullopt;

    ForestStandDrawHeads heads{};
    auto& color = heads.Color;
    color.Material = mat;
    color.VertexFlags = Rendering::VertexAttributeFlags::StandardMesh;
    color.Geometry.AltGeom = {
        bindings.coreVB, bindings.indexBuffer, static_cast<Rendering::IndexType>(bindings.indexType)};
    // The packed page ranges address the overview mesh's original LOD0 indices.
    color.IndexCount = geometry->indexCount;
    color.FirstIndex = geometry->firstIndex;
    color.VertexOffset = static_cast<int32>(geometry->vertexOffset);
    if (geometry->ForestSectorLocal)
        heads.SectorLocalMaterialIndex = mat->GetGpuSceneMaterialIndex();
    else
    {
        const auto* constants = frame.AllocatePassData<IdentityMeshDrawConstants>(mat->GetGpuSceneMaterialIndex());
        color.Bindings.PushConstants = std::as_bytes(std::span(constants, 1u));
    }
    if (!forward || forward->Purpose != ForwardEmitPurpose::World)
        return heads;

    const bool masked = mat->GetAlphaMode() == MaterialAlphaMode::Mask;
    const auto head = ChooseDepthHeadPipeline(Rendering::MaterialKeyword::None, DepthPassType::Prepass,
                                              masked, /*vertexModified=*/false, /*sharedDepthOffered=*/false,
                                              WritesReliefDepth(mat->GetVariantKey().materialKeywords));
    auto& variants = services.Materials().Variants();
    const auto* variant = variants.FindOrRequestPrepassVariant(
        *mat, color.VertexFlags, geometry->topology, head.Keywords, services.GetFrontFace());
    if (variant && variants.EnsureConcreteWarm(*mat, variant->PipelineId, services.Views().GetViewPrepassFormatKey(forward->ViewId)))
    {
        heads.Depth = color;
        heads.Depth->InternedPipeline = variant->PipelineId;
        heads.Depth->PipelineMeta = variant->VariantMeta.get();
        heads.Depth->PipelineSetCount = static_cast<uint32_t>(variant->SetLayouts.size());
        heads.Depth->PassKeywords = head.Keywords;
    }
    return heads;
}

ScopedSubscription RegisterForwardStand(RenderServices& services, const std::shared_ptr<StandEmitSource>& stand)
{
    return services.RegisterForwardEmit([stand](ForwardEmitContext& ctx) { EmitStand(*stand, ctx); },
                                        {.WritesDepth = true, .RequiresFrame = true, .SupportsCapture = true});
}

} // namespace

ScopedSubscription RegisterForestStandDraws(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, PrepareForestStandDraw prepareDraw,
    std::shared_ptr<ForestPlacementSession> details)
{
    if (!placements || !prepareDraw || layers == Rendering::RenderLayerMask::None())
        return {};
    auto stand = std::make_shared<StandEmitSource>(
        StandEmitSource{worldId, layers, std::move(placements), std::move(prepareDraw), std::move(details), {}});
    return RegisterForwardStand(services, stand);
}

ScopedSubscription RegisterForestStandMesh(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, const Rendering::MeshGPUKey& mesh,
    std::function<GUID(bool)> resolveMaterial, ScopedSubscription& shadowRegistration,
    std::shared_ptr<ForestPlacementSession> details)
{
    if (!placements || !resolveMaterial || layers == Rendering::RenderLayerMask::None())
        return {};
    const uint32 indexEnd = placements->StandIndexEnd();
    auto stand = std::make_shared<StandEmitSource>(StandEmitSource{worldId, layers, std::move(placements),
        [mesh, indexEnd, resolveMaterial](const ForwardEmitContext& ctx)
        { return PrepareStandMesh(*ctx.Services, *ctx.Frame, mesh, indexEnd, resolveMaterial(false), &ctx); }, details, {}});
    shadowRegistration = RegisterForestShadowCasters(services, worldId, layers, std::move(details),
        [stand, mesh, indexEnd, resolveMaterial = std::move(resolveMaterial)](const DepthEmitContext& ctx, uint32 cascade,
            const ForestPlacementView& view, std::span<const ForestDetailDraw> detailed)
        {
            auto heads = PrepareStandMesh(*ctx.Services, *ctx.Frame, mesh, indexEnd, resolveMaterial(true));
            if (!heads)
                return;
            auto* draws = ctx.Frame->AllocatePassData<std::vector<DrawSubCommand>>();
            stand->Placements->GatherShadowOverview(cascade, view, detailed, *draws);
            if (draws->empty() || !ApplyStandIndexRange(heads->Color, *draws))
                return;
            const auto emit = [&ctx, cascade](ForestStandDrawHeads& group, std::span<const DrawSubCommand> ranges)
            {
                group.Color.Producer = StandProducer();
                group.Color.SubDraws = ranges;
                ctx.Services->EmitCascadeDepthCommand(ctx.ViewId, cascade, group.Color);
            };
            if (heads->SectorLocalMaterialIndex)
                EmitSectorRanges(*stand, *ctx.Frame, *heads, *draws, emit);
            else
                emit(*heads, *draws);
        });
    return RegisterForwardStand(services, stand);
}

} // namespace GameEngine::Engine::Renderer
