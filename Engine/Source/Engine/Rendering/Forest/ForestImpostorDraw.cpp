#include "Engine/Rendering/Forest/ForestImpostorDraw.h"

#include "Engine/Rendering/Forest/ForestPlacementGpuData.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"

#include <array>
#include <cstring>

namespace GameEngine::Engine::Renderer
{
namespace
{

struct alignas(16) ForestCardParameters
{
    Mathematics::Vector4 Atlas;
    Mathematics::Vector4 Wind;
    std::array<uint32, 4> Material;
};
static_assert(sizeof(ForestCardParameters) == 48);

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

} // namespace

bool EmitForestImpostorDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    Rendering::BufferHandle cardIndices, const ImpostorFoliageSway& sway, DrawCommand color)
{
    using namespace Rendering::RenderGraph;
    if (!context.Frame || !context.Services || context.Purpose != ForwardEmitPurpose::World ||
        !cardIndices.IsValid() || !color.Material || !color.Material->GetCompileSpec().customVertexShader ||
        color.Material->GetCompileSpec().vertexModifierPath != "VertexModifiers/forest_impostor.glsl" ||
        color.Material->GetGpuSceneMaterialIndex() == 0xFFFFFFFFu)
        return false;

    ForestCardParameters params{};
    if (!color.Material->GetVector(HashStringId("impostorAtlas"), params.Atlas.v, 4u) || params.Atlas.x < 1.0f)
        return false;
    params.Wind = {sway.BaseY, sway.Height, sway.MaxDisplacement, 0};
    params.Material[0] = color.Material->GetGpuSceneMaterialIndex();

    auto& frame = *context.Frame;
    const auto paramUpload = frame.AllocUpload(sizeof(params));
    if (!paramUpload.Valid())
        return false;
    std::memcpy(paramUpload.Ptr, &params, sizeof(params));
    const auto parameterBuffer = frame.ImportExternalBuffer("Forest.CardParameters", paramUpload.Buffer);
    const auto args = frame.PhysicalBuffer(buffers.Args);
    auto& bindings = *frame.AllocatePassData<std::array<DrawBindings::BufferEntry, 5>>();
    bindings = {{
        {HashStringId("ForestPlacementSlots"), frame.PhysicalBuffer(buffers.Slots), 0, buffers.SlotBytes},
        {HashStringId("ForestPlacementPages"), frame.PhysicalBuffer(buffers.Pages), buffers.PagesOffset, buffers.PagesBytes},
        {HashStringId("ForestVisiblePlacements"), frame.PhysicalBuffer(buffers.Visible), 0, buffers.VisibleBytes},
        {HashStringId("forestView"), frame.PhysicalBuffer(buffers.View), buffers.ViewOffset, sizeof(ForestPlacementGpuView)},
        {HashStringId("forestCard"), paramUpload.Buffer, paramUpload.Offset, sizeof(params)},
    }};

    constexpr auto readers = ForwardBufferReaders::WorldPass;
    auto& services = *context.Services;
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Slots, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Pages, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Visible, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.View, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, parameterBuffer, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Args, RGBufferRead::Indirect, readers);
    BindForestCard(color, cardIndices, args, bindings);
    services.EmitForwardCommand(context.ViewId, color, ForwardDrawDepth::ColourPass);
    return true;
}

} // namespace GameEngine::Engine::Renderer
