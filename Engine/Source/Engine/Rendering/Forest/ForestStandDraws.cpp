#include "ForestStandDraws.h"

#include "../IdentityMeshDrawConstants.h"
#include "ForestStandDrawProducer.h"

#include "ForestPlacementRuntime.h"
#include "ForestPlacementFeature.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
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

ForestPlacementView MakeStandView(const Rendering::CameraData& camera)
{
    ForestPlacementView view{CameraEye(camera), {}};
    Rendering::ExtractFrustumPlanes(camera.viewProjRel, view.EyeRelativePlanes.data());
    const Mathematics::Vector3 eye = CameraEyeRelativeToOrigin(camera);
    for (auto& plane : view.EyeRelativePlanes)
        plane.w += plane.x * eye.x + plane.y * eye.y + plane.z * eye.z;
    return view;
}

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

// The camera of a view of the stand's world that shows one of its layers.
const Rendering::CameraData* FindStandCamera(const ForwardEmitContext& context, const StandEmitSource& stand)
{
    if (!context.Frame)
        return nullptr;
    auto& views = context.Services->Views();
    const auto* view = views.FindViewDesc(context.ViewId);
    if (!view || view->worldId != stand.WorldId || !stand.Layers.Intersects(view->ActiveRenderLayerMask()))
        return nullptr;
    return views.FindCameraData(view->cameraId);
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

void EmitStandCommands(ForwardEmitContext& context, ForestStandDrawHeads& heads, std::span<const DrawSubCommand> draws)
{
    heads.Color.Producer = StandProducer();
    heads.Color.SubDraws = draws;
    if (!heads.Depth || context.Purpose != ForwardEmitPurpose::World)
    {
        context.Services->EmitForwardCommand(context.ViewId, heads.Color, ForwardDrawDepth::ColourPass);
        return;
    }
    heads.Depth->Producer = StandProducer();
    heads.Depth->SubDraws = draws;
    context.Services->EmitForwardCommand(context.ViewId, heads.Color, ForwardDrawDepth::Prepass, &*heads.Depth);
}

void EmitStand(const StandEmitSource& stand, ForwardEmitContext& context)
{
    const auto* camera = FindStandCamera(context, stand);
    if (!camera)
        return;

    auto* draws = context.Frame->AllocatePassData<std::vector<DrawSubCommand>>();
    const auto placementView = MakeStandView(*camera);
    const bool usesDetails = stand.Details && context.Purpose == ForwardEmitPurpose::World;
    if (!usesDetails)
    {
        stand.Placements->GatherStand(placementView, *draws);
        if (draws->empty())
            return;
    }
    auto heads = stand.PrepareDraw(context);
    if (!heads || !AreDrawableHeads(*heads))
        return;

    if (usesDetails)
    {
        auto* detailFeature = context.Services->GetFeature<ForestPlacementFeature>();
        if (!detailFeature || !context.Frame->RequireExecutionLifetime(stand.Details) ||
            !detailFeature->EmitDetails(*stand.Details, context, *camera, placementView, *draws))
            stand.Placements->GatherStand(placementView, *draws);
    }
    if (draws->empty() || !ApplyStandIndexRange(heads->Color, *draws))
        return;
    EmitStandCommands(context, *heads, *draws);
}

// The shaded head of the overview mesh, or none while the mesh or its material is not drawable.
std::optional<ForestStandDrawHeads> PrepareStandMesh(const ForwardEmitContext& context, const Rendering::MeshGPUKey& mesh,
                                                     uint32 indexEnd, const GUID& material)
{
    if (material.IsNull())
        return std::nullopt;
    auto& registry = context.Services->GetMeshGPURegistry();
    const Rendering::MeshGPURegistry::TableScope scope(registry);
    const auto* geometry = registry.FindByKey(mesh);
    const auto* mat = context.Services->Materials().Registry().Find(material);
    Rendering::MeshGPUEntryBindings bindings{};
    if (!mat || !geometry || geometry->vertexFlags != Rendering::VertexAttributeFlags::StandardMesh ||
        geometry->topology != Rendering::PrimitiveTopology::TriangleList ||
        geometry->indexCount != indexEnd ||
        !mat->GetCompileSpec().vertexModifierPath.empty() || mat->GetCompileSpec().customVertexShader ||
        (mat->GetAlphaMode() != MaterialAlphaMode::Mask && mat->GetAlphaMode() != MaterialAlphaMode::Opaque) ||
        geometry->vertexOffset > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
        !registry.TryGetDrawableBindings(*geometry, bindings))
        return std::nullopt;

    const auto* transform = context.Frame->AllocatePassData<IdentityMeshDrawConstants>(mat->GetGpuSceneMaterialIndex());
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
    if (context.Purpose != ForwardEmitPurpose::World)
        return heads;

    auto& services = *context.Services;
    const bool masked = mat->GetAlphaMode() == MaterialAlphaMode::Mask;
    const auto head = ChooseDepthHeadPipeline(Rendering::MaterialKeyword::None, DepthPassType::Prepass,
                                              masked, /*vertexModified=*/false, /*sharedDepthOffered=*/false,
                                              WritesReliefDepth(mat->GetVariantKey().materialKeywords));
    auto& variants = services.Materials().Variants();
    const auto* variant = variants.FindOrRequestPrepassVariant(
        *mat, color.VertexFlags, geometry->topology, head.Keywords, services.GetFrontFace());
    if (variant && variants.EnsureConcreteWarm(*mat, variant->PipelineId, services.Views().GetViewPrepassFormatKey(context.ViewId)))
    {
        heads.Depth = color;
        heads.Depth->InternedPipeline = variant->PipelineId;
        heads.Depth->PipelineMeta = variant->VariantMeta.get();
        heads.Depth->PassKeywords = head.Keywords;
    }
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
    return services.RegisterForwardEmit([stand](ForwardEmitContext& context) { EmitStand(*stand, context); },
                                        {.WritesDepth = true, .RequiresFrame = true, .SupportsCapture = true});
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
        [mesh, indexEnd, resolveMaterial = std::move(resolveMaterial)](const ForwardEmitContext& context)
        { return PrepareStandMesh(context, mesh, indexEnd, resolveMaterial()); },
        std::move(details));
}

} // namespace GameEngine::Engine::Renderer
