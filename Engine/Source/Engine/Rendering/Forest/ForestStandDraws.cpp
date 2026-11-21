#include "Engine/Rendering/Forest/ForestStandDraws.h"

#include "../IdentityMeshDrawConstants.h"

#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Engine/Rendering/Forest/ForestPlacementFeature.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

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

ForestPlacementView MakeStandView(const Rendering::CameraData& camera)
{
    ForestPlacementView view{CameraEye(camera), {}};
    Rendering::ExtractFrustumPlanes(camera.viewProjRel, view.EyeRelativePlanes.data());
    const Mathematics::Vector3 eye = CameraEyeRelativeToOrigin(camera);
    for (auto& plane : view.EyeRelativePlanes)
        plane.w += plane.x * eye.x + plane.y * eye.y + plane.z * eye.z;
    return view;
}

} // namespace

ScopedSubscription RegisterForestStandDraws(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, PrepareForestStandDraw prepareDraw,
    std::shared_ptr<ForestPlacementSession> details)
{
    if (!placements || !prepareDraw || layers == Rendering::RenderLayerMask::None())
        return {};
    const auto producer = Rendering::DrawProducerRegistry::Register("ForestStand");
    const bool usesPackedDetails = details != nullptr;
    return services.RegisterForwardEmit(
        [worldId, layers, producer, placements = std::move(placements), prepareDraw = std::move(prepareDraw),
         details = std::move(details)](
            ForwardEmitContext& ctx)
        {
            if (!ctx.Frame || ctx.Purpose != ForwardEmitPurpose::World)
                return;
            auto& views = ctx.Services->Views();
            const auto* view = views.FindViewDesc(ctx.ViewId);
            if (!view || view->worldId != worldId || !layers.Intersects(view->ActiveRenderLayerMask()))
                return;
            const auto* camera = views.FindCameraData(view->cameraId);
            if (!camera)
                return;

            auto* draws = ctx.Frame->AllocatePassData<std::vector<DrawSubCommand>>();
            const auto placementView = MakeStandView(*camera);
            if (!details)
            {
                placements->GatherStand(placementView, *draws);
                if (draws->empty())
                    return;
            }
            auto heads = prepareDraw(ctx);
            if (!heads || heads->Color.UseIndirect || (heads->Depth && heads->Depth->UseIndirect))
                return;
            if (heads->Depth &&
                (heads->Depth->FirstIndex != heads->Color.FirstIndex ||
                 heads->Depth->VertexOffset != heads->Color.VertexOffset ||
                 heads->Depth->IndexCount != heads->Color.IndexCount))
                return;

            std::vector<ForestStandFadeDraw> fades;
            if (details)
            {
                auto* detailFeature = ctx.Services->GetFeature<ForestPlacementFeature>();
                bool fadeReady = true;
                if (ForestSplatHandoffEnabled())
                {
                    const auto pass = views.GetViewDeclaredWorldPassKeywords(ctx.ViewId);
                    const auto format = views.GetViewWorldColorFormatKey(ctx.ViewId);
                    fadeReady = pass && format.ColorCount != 0 && !heads->Depth && heads->Color.Material &&
                        heads->Color.Bindings.PushConstants.size() == sizeof(IdentityMeshDrawConstants) &&
                        ctx.Services->Materials().Variants().WarmColorVariant(*heads->Color.Material,
                            heads->Color.VertexFlags, Rendering::PrimitiveTopology::TriangleList,
                            (*pass & ~Rendering::MaterialKeyword::Instanced) | Rendering::MaterialKeyword::LodCrossfade,
                            ctx.Services->GetFrontFace(), format);
                }
                if (!detailFeature || !fadeReady || !ctx.Frame->RequireExecutionLifetime(details) ||
                    !detailFeature->EmitDetails(*details, ctx, *camera, placementView, *draws, fades))
                    placements->GatherStand(placementView, *draws);
            }
            for (auto& fade : fades)
            {
                auto color = heads->Color;
                auto ranges = std::span(&fade.Range, 1);
                if (!ApplyStandIndexRange(color, ranges))
                    continue;
                auto* range = ctx.Frame->AllocatePassData<DrawSubCommand>(fade.Range);
                auto* constants = ctx.Frame->AllocatePassData<IdentityMeshDrawConstants>(0u);
                std::memcpy(constants, color.Bindings.PushConstants.data(), sizeof(*constants));
                constants->Reserved0 = fade.FadeCode;
                color.Bindings.PushConstants = std::as_bytes(std::span(constants, 1));
                color.PassKeywords |= Rendering::MaterialKeyword::LodCrossfade;
                color.Producer = producer;
                color.SubDraws = std::span(range, 1);
                ctx.Services->EmitForwardCommand(ctx.ViewId, color, ForwardDrawDepth::ColourPass);
            }
            if (draws->empty() || !ApplyStandIndexRange(heads->Color, *draws))
                return;
            heads->Color.Producer = producer;
            heads->Color.SubDraws = *draws;
            if (heads->Depth)
            {
                heads->Depth->Producer = producer;
                heads->Depth->SubDraws = *draws;
                ctx.Services->EmitForwardCommand(ctx.ViewId, heads->Color, ForwardDrawDepth::Prepass, &*heads->Depth);
            }
            else
            {
                ctx.Services->EmitForwardCommand(ctx.ViewId, heads->Color, ForwardDrawDepth::ColourPass);
            }
        },
        {.WritesDepth = true, .RequiresFrame = true, .NeedsWorldPassFormats = usesPackedDetails});
}

ScopedSubscription RegisterForestStandMesh(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, const Rendering::MeshGPUKey& mesh,
    std::function<GUID()> resolveMaterial, std::shared_ptr<ForestPlacementSession> details)
{
    if (!placements || !resolveMaterial)
        return {};
    const uint32 indexEnd = placements->StandIndexEnd();
    return RegisterForestStandDraws(services, worldId, layers, std::move(placements),
        [mesh, indexEnd, resolveMaterial = std::move(resolveMaterial)](const ForwardEmitContext& ctx) -> std::optional<ForestStandDrawHeads>
        {
            const GUID material = resolveMaterial();
            if (material.IsNull())
                return std::nullopt;
            auto& registry = ctx.Services->GetMeshGPURegistry();
            const Rendering::MeshGPURegistry::TableScope scope(registry);
            const auto* geometry = registry.FindByKey(mesh);
            const auto* mat = ctx.Services->Materials().Registry().Find(material);
            Rendering::MeshGPUEntryBindings bindings{};
            if (!mat || !geometry || geometry->vertexFlags != Rendering::VertexAttributeFlags::StandardMesh ||
                geometry->topology != Rendering::PrimitiveTopology::TriangleList ||
                geometry->indexCount != indexEnd || geometry->lodCount != 1 ||
                !mat->GetCompileSpec().vertexModifierPath.empty() || mat->GetCompileSpec().customVertexShader ||
                (mat->GetAlphaMode() != MaterialAlphaMode::Mask && mat->GetAlphaMode() != MaterialAlphaMode::Opaque) ||
                geometry->vertexOffset > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
                !registry.TryGetDrawableBindings(*geometry, bindings))
                return std::nullopt;

            const auto* transform = ctx.Frame->AllocatePassData<IdentityMeshDrawConstants>(
                mat->GetGpuSceneMaterialIndex());
            ForestStandDrawHeads heads{};
            auto& color = heads.Color;
            color.Material = mat;
            color.VertexFlags = Rendering::VertexAttributeFlags::StandardMesh;
            color.Geometry.AltGeom = {
                bindings.coreVB, bindings.indexBuffer, static_cast<Rendering::IndexType>(bindings.indexType)};
            color.IndexCount = geometry->indexCount;
            color.FirstIndex = geometry->firstIndex;
            color.VertexOffset = static_cast<int32>(geometry->vertexOffset);
            color.Bindings.PushConstants = std::as_bytes(std::span(transform, 1u));
            return heads;
        }, std::move(details));
}

} // namespace GameEngine::Engine::Renderer
