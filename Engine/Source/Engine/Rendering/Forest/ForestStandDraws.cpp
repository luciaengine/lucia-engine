#include "ForestStandDraws.h"

#include "../IdentityMeshDrawConstants.h"
#include "ForestStandDrawProducer.h"

#include "ForestPlacementRuntime.h"
#include "ForestPlacementFeature.h"
#include "ForestPlacementSession.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

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

// What one registered stand draws, shared by its forward-emit callback.
struct StandEmitSource
{
    uint64 WorldId;
    Rendering::RenderLayerMask Layers;
    std::shared_ptr<ForestPlacementRuntime> Placements;
    PrepareForestStandDraw PrepareDraw;
    std::shared_ptr<ForestPlacementSession> Details;
};

Rendering::DrawProducerId StandProducer()
{
    static const auto producer = Rendering::DrawProducerRegistry::Register("ForestStand");
    return producer;
}

// The camera of a world view of the stand's world that shows one of its layers, else null.
const Rendering::CameraData* FindStandCamera(const ForwardEmitContext& ctx, const StandEmitSource& stand)
{
    if (!ctx.Frame || ctx.Purpose != ForwardEmitPurpose::World)
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
    if (!heads.Depth)
    {
        ctx.Services->EmitForwardCommand(ctx.ViewId, heads.Color, ForwardDrawDepth::ColourPass);
        return;
    }
    heads.Depth->Producer = StandProducer();
    heads.Depth->SubDraws = draws;
    ctx.Services->EmitForwardCommand(ctx.ViewId, heads.Color, ForwardDrawDepth::Prepass, &*heads.Depth);
}

void EmitStand(const StandEmitSource& stand, ForwardEmitContext& ctx)
{
    const auto* camera = FindStandCamera(ctx, stand);
    if (!camera || (stand.Details && !stand.Details->DrawsColor))
        return;

    auto* draws = ctx.Frame->AllocatePassData<std::vector<DrawSubCommand>>();
    const auto placementView = MakeForestStandView(*camera);
    if (!stand.Details)
    {
        stand.Placements->GatherStand(placementView, *draws);
        if (draws->empty())
            return;
    }
    auto heads = stand.PrepareDraw(ctx);
    if (!heads || !AreDrawableHeads(*heads))
        return;

    if (stand.Details)
    {
        auto* detailFeature = ctx.Services->GetFeature<ForestPlacementFeature>();
        if (!detailFeature || !ctx.Frame->RequireExecutionLifetime(stand.Details) ||
            !detailFeature->EmitDetails(*stand.Details, ctx, *camera, placementView, *draws))
            stand.Placements->GatherStand(placementView, *draws);
    }
    if (draws->empty() || !ApplyStandIndexRange(heads->Color, *draws))
        return;
    EmitStandCommands(ctx, *heads, *draws);
}

// The shaded head of the overview mesh, or none while the mesh or its material is not drawable.
std::optional<ForestStandDrawHeads> PrepareStandMesh(const ForwardEmitContext& ctx, const Rendering::MeshGPUKey& mesh,
                                                     uint32 indexEnd, const GUID& material)
{
    if (material.IsNull())
        return std::nullopt;
    auto& registry = ctx.Services->GetMeshGPURegistry();
    const Rendering::MeshGPURegistry::TableScope scope(registry);
    const auto* geometry = registry.FindByKey(mesh);
    const auto* mat = ctx.Services->Materials().Registry().Find(material);
    Rendering::MeshGPUEntryBindings bindings{};
    if (!mat || !geometry || geometry->vertexFlags != Rendering::VertexAttributeFlags::StandardMesh ||
        geometry->topology != Rendering::PrimitiveTopology::TriangleList ||
        geometry->indexCount != indexEnd ||
        !mat->GetCompileSpec().vertexModifierPath.empty() || mat->GetCompileSpec().customVertexShader ||
        (mat->GetAlphaMode() != MaterialAlphaMode::Mask && mat->GetAlphaMode() != MaterialAlphaMode::Opaque) ||
        geometry->vertexOffset > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
        !registry.TryGetDrawableBindings(*geometry, bindings))
        return std::nullopt;

    const auto* transform = ctx.Frame->AllocatePassData<IdentityMeshDrawConstants>(mat->GetGpuSceneMaterialIndex());
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
    color.Bindings.PushConstants = std::as_bytes(std::span(transform, 1u));
    return heads;
}

} // namespace

ScopedSubscription RegisterForestStandDraws(
    RenderServices& services, uint64 worldId, Rendering::RenderLayerMask layers,
    std::shared_ptr<ForestPlacementRuntime> placements, PrepareForestStandDraw prepareDraw,
    std::shared_ptr<ForestPlacementSession> details)
{
    if (!placements || !prepareDraw || layers == Rendering::RenderLayerMask::None())
        return {};
    auto stand = std::make_shared<const StandEmitSource>(
        StandEmitSource{worldId, layers, std::move(placements), std::move(prepareDraw), std::move(details)});
    return services.RegisterForwardEmit([stand](ForwardEmitContext& ctx) { EmitStand(*stand, ctx); },
                                        {.WritesDepth = true, .RequiresFrame = true});
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
        [mesh, indexEnd, resolveMaterial = std::move(resolveMaterial)](const ForwardEmitContext& ctx)
        { return PrepareStandMesh(ctx, mesh, indexEnd, resolveMaterial()); },
        std::move(details));
}

} // namespace GameEngine::Engine::Renderer
