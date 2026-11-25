#include "ForestImpostorDraw.h"

#include "ForestPlacementGpuData.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/Forest/ForestSplatCells.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace GameEngine::Engine::Renderer
{
namespace
{

struct alignas(16) ForestCardParameters
{
    Mathematics::Vector4 Atlas;
    Mathematics::Vector4 Wind;
    std::array<uint32, 4> Material;
    Mathematics::Vector4 Viewer;
};
static_assert(sizeof(ForestCardParameters) == 64);

// The buffers that a card draw binds and the graph buffers that its pass reads.
struct ForestCardBindings
{
    std::span<const DrawBindings::BufferEntry> Buffers;
    Rendering::RenderGraph::RGBuffer Parameters;
    Rendering::BufferHandle Args;
};

void BindForestCard(DrawCommand& command, Rendering::BufferHandle indices, Rendering::BufferHandle args,
                    std::span<const DrawBindings::BufferEntry> buffers)
{
    command.VertexFlags = Rendering::VertexAttributeFlags::None;
    command.Geometry = {};
    command.Geometry.AltGeom = {{}, indices, Rendering::IndexType::Uint16};
    command.UseIndirect = true;
    command.FixedIndirectCount = true;
    command.IndirectCommandBuffer = args;
    command.IndirectMaxDrawCount = 1;
    command.IndirectStride = sizeof(ForestPlacementIndirectDraw);
    command.Bindings.Buffers = buffers;
    static const auto producer = Rendering::DrawProducerRegistry::Register("ForestImpostor");
    command.Producer = producer;
}

// True when the material is a packed forest material of this vertex modifier with a GPU scene slot.
bool IsForestDrawMaterial(const Material* material, std::string_view vertexModifier)
{
    return material && material->GetCompileSpec().customVertexShader &&
           material->GetCompileSpec().vertexModifierPath == vertexModifier &&
           material->GetGpuSceneMaterialIndex() != 0xFFFFFFFFu;
}

std::optional<ForestCardBindings> PrepareForestBindings(Rendering::RenderGraph::RGFrame& frame,
    const ForestPlacementCull::DrawBuffers& buffers, const ForestCardParameters& params,
    const DrawBindings::BufferEntry* extra = nullptr)
{
    const auto paramUpload = frame.AllocUpload(sizeof(params));
    if (!paramUpload.Valid())
        return std::nullopt;
    std::memcpy(paramUpload.Ptr, &params, sizeof(params));
    const auto parameterBuffer = frame.ImportExternalBuffer("Forest.CardParameters", paramUpload.Buffer);
    auto& bindings = *frame.AllocatePassData<std::array<DrawBindings::BufferEntry, 6>>();
    bindings = {{
        {HashStringId("ForestPlacementSlots"), frame.PhysicalBuffer(buffers.Slots), 0, buffers.SlotBytes},
        {HashStringId("ForestPlacementPages"), frame.PhysicalBuffer(buffers.Pages), buffers.PagesOffset, buffers.PagesBytes},
        {HashStringId("ForestVisiblePlacements"), frame.PhysicalBuffer(buffers.Visible), 0, buffers.VisibleBytes},
        {HashStringId("forestView"), frame.PhysicalBuffer(buffers.View), buffers.ViewOffset, sizeof(ForestPlacementGpuView)},
        {HashStringId("forestCard"), paramUpload.Buffer, paramUpload.Offset, sizeof(params)},
    }};
    if (extra)
        bindings[5] = *extra;
    return ForestCardBindings{std::span(bindings).first(extra ? 6u : 5u), parameterBuffer, frame.PhysicalBuffer(buffers.Args)};
}

std::optional<ForestCardBindings> PrepareForestCard(Rendering::RenderGraph::RGFrame& frame,
    const ForestPlacementCull::DrawBuffers& buffers, const Material& material, const ImpostorFoliageSway& sway,
    const Mathematics::Vector4& viewer)
{
    ForestCardParameters params{};
    if (!material.GetVector(HashStringId("impostorAtlas"), params.Atlas.v, 4u) || params.Atlas.x < 1.0f)
        return std::nullopt;
    params.Wind = {sway.BaseY, sway.Height, sway.MaxDisplacement, 0};
    params.Material[0] = material.GetGpuSceneMaterialIndex();
    params.Viewer = viewer;

    return PrepareForestBindings(frame, buffers, params);
}

bool EmitForestPlacementDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    Rendering::BufferHandle indices, const ForestCardBindings& card, const DrawBindings::BufferEntry* extra,
    DrawCommand color)
{
    using namespace Rendering::RenderGraph;
    auto& frame = *context.Frame;
    auto& services = *context.Services;
    constexpr auto readers = ForwardBufferReaders::WorldPass;
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Slots, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Pages, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Visible, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.View, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, card.Parameters, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Args, RGBufferRead::Indirect, readers);
    if (extra)
    {
        const auto imported = frame.ImportExternalBuffer("Forest.SplatCells", extra->Buffer, extra->Range);
        services.EmitForwardSampledBufferRead(frame, context.ViewId, imported, RGBufferRead::Storage, readers);
    }
    BindForestCard(color, indices, card.Args, card.Buffers);
    services.EmitForwardCommand(context.ViewId, color, ForwardDrawDepth::ColourPass);
    return true;
}

} // namespace

Mathematics::Vector4 ForestCardEyeViewer(const Rendering::CameraData& camera)
{
    return {CameraEyeRelativeToOrigin(camera), 1.0f};
}

bool EmitForestImpostorDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    Rendering::BufferHandle cardIndices, const ImpostorFoliageSway& sway, DrawCommand color)
{
    using namespace Rendering::RenderGraph;
    if (!context.Frame || !context.Services || context.Purpose != ForwardEmitPurpose::World ||
        !cardIndices.IsValid() || !IsForestDrawMaterial(color.Material, "VertexModifiers/forest_impostor.glsl"))
        return false;
    auto& services = *context.Services;
    const auto* view = services.Views().FindViewDesc(context.ViewId);
    const auto* camera = view ? services.Views().FindCameraData(view->cameraId) : nullptr;
    if (!camera || camera->renderOriginSector[3] == 0)
        return false;

    auto& frame = *context.Frame;
    const auto card = PrepareForestCard(frame, buffers, *color.Material, sway, ForestCardEyeViewer(*camera));
    if (!card)
        return false;
    return EmitForestPlacementDraw(context, buffers, cardIndices, *card, nullptr, color);
}

bool EmitForestImpostorShadowDraw(const DepthEmitContext& context, uint32 cascade,
    const ForestPlacementCull::DrawBuffers& buffers, Rendering::BufferHandle cardIndices,
    const ImpostorFoliageSway& sway, DrawCommand card)
{
    using namespace Rendering::RenderGraph;
    if (!context.Frame || !context.Services || !context.Cascades || cascade >= context.Cascades->Count ||
        !cardIndices.IsValid() || !IsForestDrawMaterial(card.Material, "VertexModifiers/forest_impostor.glsl"))
        return false;

    auto& services = *context.Services;
    const auto& material = *card.Material;
    constexpr auto pass = DepthPassType::ShadowCascade;
    const auto materialKeywords = material.GetVariantKey().materialKeywords;
    if (FilterDepthPassMaterial(pass, material.GetAlphaMode(),
                                HasKeyword(materialKeywords, MaterialKeyword::Transmission)) != DepthPassMaterialFilter::Draws)
        return false;
    constexpr auto vertexFlags = Rendering::VertexAttributeFlags::None;
    const auto head = ChooseContributorDepthHead(material, pass, card.PassKeywords, vertexFlags, vertexFlags);
    // Keep the overview until the card has a concrete pipeline and alpha texture bindings.
    if (!services.Materials().Variants().WarmCascadeVariant(material, head.VertexFlags,
            Rendering::PrimitiveTopology::TriangleList, head.Head.Keywords, services.GetFrontFace()) ||
        (head.ReadsTextures && !services.Textures().IsMaterialTextureBindingComplete(material.GetGuid())))
        return false;

    // A cascade is orthographic: every card faces the light along one direction (w = 0).
    auto& frame = *context.Frame;
    const auto prepared = PrepareForestCard(frame, buffers, *card.Material, sway,
                                            {context.Cascades->TowardLight, 0.0f});
    if (!prepared)
        return false;
    services.EmitCascadeDepthBufferRead(frame, context.ViewId, cascade, buffers.Slots, RGBufferRead::Storage);
    services.EmitCascadeDepthBufferRead(frame, context.ViewId, cascade, buffers.Pages, RGBufferRead::Storage);
    services.EmitCascadeDepthBufferRead(frame, context.ViewId, cascade, buffers.Visible, RGBufferRead::Storage);
    services.EmitCascadeDepthBufferRead(frame, context.ViewId, cascade, buffers.View, RGBufferRead::Uniform);
    services.EmitCascadeDepthBufferRead(frame, context.ViewId, cascade, prepared->Parameters, RGBufferRead::Uniform);
    services.EmitCascadeDepthBufferRead(frame, context.ViewId, cascade, buffers.Args, RGBufferRead::Indirect);
    BindForestCard(card, cardIndices, prepared->Args, prepared->Buffers);
    static const auto producer = Rendering::DrawProducerRegistry::Register("ForestImpostorShadow");
    card.Producer = producer;
    services.EmitCascadeDepthCommand(context.ViewId, cascade, card);
    return true;
}

bool EmitForestSplatDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    const ForestSplatGeometry& splats, DrawCommand color, const ForestSplatGeometry& coarse)
{
    if (!context.Frame || !context.Services || context.Purpose != ForwardEmitPurpose::World || !splats.Cells ||
        !splats.CellBuffer.IsValid() || !splats.Indices.IsValid() ||
        !IsForestDrawMaterial(color.Material, "VertexModifiers/forest_splats.glsl"))
        return false;

    const auto prepare = [&context, &color](const ForestSplatGeometry& geometry,
        const ForestPlacementCull::DrawBuffers& drawBuffers, DrawBindings::BufferEntry& cellBinding) {
        const auto& cells = *geometry.Cells;
        ForestCardParameters params{};
        params.Atlas = {cells.Origin.x, cells.Origin.y, cells.Origin.z, cells.CellSize};
        params.Material[0] = color.Material->GetGpuSceneMaterialIndex();
        cellBinding = {HashStringId("ForestSplatCells"), geometry.CellBuffer, 0,
                       cells.Cells.size() * sizeof(ForestSplatCell)};
        return PrepareForestBindings(*context.Frame, drawBuffers, params, &cellBinding);
    };
    DrawBindings::BufferEntry fineCellBinding{};
    const auto fineBindings = prepare(splats, buffers, fineCellBinding);
    if (!fineBindings)
        return false;
    if (buffers.CoarseArgs.IsValid())
    {
        if (!coarse.Cells || !coarse.CellBuffer.IsValid() || !coarse.Indices.IsValid())
            return false;
        auto coarseBuffers = buffers;
        coarseBuffers.Visible = buffers.CoarseVisible;
        coarseBuffers.Args = buffers.CoarseArgs;
        DrawBindings::BufferEntry coarseCellBinding{};
        const auto coarseBindings = prepare(coarse, coarseBuffers, coarseCellBinding);
        if (!coarseBindings)
            return false;
        EmitForestPlacementDraw(context, coarseBuffers, coarse.Indices, *coarseBindings, &coarseCellBinding, color);
    }
    return EmitForestPlacementDraw(context, buffers, splats.Indices, *fineBindings, &fineCellBinding, color);
}

} // namespace GameEngine::Engine::Renderer
