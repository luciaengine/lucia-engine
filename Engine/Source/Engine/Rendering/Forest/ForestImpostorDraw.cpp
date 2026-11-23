#include "ForestImpostorDraw.h"

#include "ForestPlacementGpuData.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"

#include <array>
#include <cstring>
#include <optional>
#include <span>

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

// True when `material` is a packed card material that the GPU scene knows.
bool IsPackedCardMaterial(const Material* material)
{
    return material && material->GetCompileSpec().customVertexShader &&
           material->GetCompileSpec().vertexModifierPath == "VertexModifiers/forest_impostor.glsl" &&
           material->GetGpuSceneMaterialIndex() != 0xFFFFFFFFu;
}

// Uploads the card parameters for `viewer` and allocates the draw's bindings in the frame.
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

    const auto paramUpload = frame.AllocUpload(sizeof(params));
    if (!paramUpload.Valid())
        return std::nullopt;
    std::memcpy(paramUpload.Ptr, &params, sizeof(params));
    const auto parameterBuffer = frame.ImportExternalBuffer("Forest.CardParameters", paramUpload.Buffer);
    auto& bindings = *frame.AllocatePassData<std::array<DrawBindings::BufferEntry, 5>>();
    bindings = {{
        {HashStringId("ForestPlacementSlots"), frame.PhysicalBuffer(buffers.Slots), 0, buffers.SlotBytes},
        {HashStringId("ForestPlacementPages"), frame.PhysicalBuffer(buffers.Pages), buffers.PagesOffset, buffers.PagesBytes},
        {HashStringId("ForestVisiblePlacements"), frame.PhysicalBuffer(buffers.Visible), 0, buffers.VisibleBytes},
        {HashStringId("forestView"), frame.PhysicalBuffer(buffers.View), buffers.ViewOffset, sizeof(ForestPlacementGpuView)},
        {HashStringId("forestCard"), paramUpload.Buffer, paramUpload.Offset, sizeof(params)},
    }};
    return ForestCardBindings{bindings, parameterBuffer, frame.PhysicalBuffer(buffers.Args)};
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
        !cardIndices.IsValid() || !IsPackedCardMaterial(color.Material))
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
    constexpr auto readers = ForwardBufferReaders::WorldPass;
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Slots, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Pages, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Visible, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.View, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, card->Parameters, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Args, RGBufferRead::Indirect, readers);
    BindForestCard(color, cardIndices, card->Args, card->Buffers);
    services.EmitForwardCommand(context.ViewId, color, ForwardDrawDepth::ColourPass);
    return true;
}

bool EmitForestImpostorShadowDraw(const DepthEmitContext& context, uint32 cascade,
    const ForestPlacementCull::DrawBuffers& buffers, Rendering::BufferHandle cardIndices,
    const ImpostorFoliageSway& sway, DrawCommand card)
{
    using namespace Rendering::RenderGraph;
    if (!context.Frame || !context.Services || !context.Cascades || cascade >= context.Cascades->Count ||
        !cardIndices.IsValid() || !IsPackedCardMaterial(card.Material))
        return false;

    // A cascade is orthographic: every card faces the light along one direction (w = 0).
    auto& frame = *context.Frame;
    const auto prepared = PrepareForestCard(frame, buffers, *card.Material, sway,
                                            {context.Cascades->TowardLight, 0.0f});
    if (!prepared)
        return false;
    auto& services = *context.Services;
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

} // namespace GameEngine::Engine::Renderer
