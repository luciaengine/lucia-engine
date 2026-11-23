#include "ForestImpostorDraw.h"

#include "ForestPlacementGpuData.h"
#include "Engine/Rendering/Forest/ForestSplatCells.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"

#include <array>
#include <cstring>
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

// True when the material is a packed forest material of this vertex modifier with a GPU scene slot.
bool IsForestDrawMaterial(const Material* material, std::string_view vertexModifier)
{
    return material && material->GetCompileSpec().customVertexShader &&
           material->GetCompileSpec().vertexModifierPath == vertexModifier &&
           material->GetGpuSceneMaterialIndex() != 0xFFFFFFFFu;
}

// Binds the culled placements, the parameters and the extra buffer, and emits the color draw.
bool EmitForestPlacementDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    Rendering::BufferHandle indices, const ForestCardParameters& params, const DrawBindings::BufferEntry* extra,
    DrawCommand color)
{
    using namespace Rendering::RenderGraph;
    auto& frame = *context.Frame;
    const auto paramUpload = frame.AllocUpload(sizeof(params));
    if (!paramUpload.Valid())
        return false;
    std::memcpy(paramUpload.Ptr, &params, sizeof(params));
    const auto parameterBuffer = frame.ImportExternalBuffer("Forest.CardParameters", paramUpload.Buffer);
    const auto args = frame.PhysicalBuffer(buffers.Args);
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

    constexpr auto readers = ForwardBufferReaders::WorldPass;
    auto& services = *context.Services;
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Slots, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Pages, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Visible, RGBufferRead::Storage, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.View, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, parameterBuffer, RGBufferRead::Uniform, readers);
    services.EmitForwardSampledBufferRead(frame, context.ViewId, buffers.Args, RGBufferRead::Indirect, readers);
    if (extra)
    {
        const auto imported = frame.ImportExternalBuffer("Forest.SplatCells", extra->Buffer, extra->Range);
        services.EmitForwardSampledBufferRead(frame, context.ViewId, imported, RGBufferRead::Storage, readers);
    }
    BindForestCard(color, indices, args, std::span(bindings).first(extra ? 6u : 5u));
    services.EmitForwardCommand(context.ViewId, color, ForwardDrawDepth::ColourPass);
    return true;
}

} // namespace

bool EmitForestImpostorDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    Rendering::BufferHandle cardIndices, const ImpostorFoliageSway& sway, DrawCommand color)
{
    if (!context.Frame || !context.Services || context.Purpose != ForwardEmitPurpose::World ||
        !cardIndices.IsValid() || !IsForestDrawMaterial(color.Material, "VertexModifiers/forest_impostor.glsl"))
        return false;

    ForestCardParameters params{};
    if (!color.Material->GetVector(HashStringId("impostorAtlas"), params.Atlas.v, 4u) || params.Atlas.x < 1.0f)
        return false;
    params.Wind = {sway.BaseY, sway.Height, sway.MaxDisplacement, 0};
    params.Material[0] = color.Material->GetGpuSceneMaterialIndex();
    return EmitForestPlacementDraw(context, buffers, cardIndices, params, nullptr, color);
}

bool EmitForestSplatDraw(const ForwardEmitContext& context, const ForestPlacementCull::DrawBuffers& buffers,
    const ForestSplatGeometry& splats, DrawCommand color)
{
    if (!context.Frame || !context.Services || context.Purpose != ForwardEmitPurpose::World || !splats.Cells ||
        !splats.CellBuffer.IsValid() || !splats.Indices.IsValid() ||
        !IsForestDrawMaterial(color.Material, "VertexModifiers/forest_splats.glsl"))
        return false;

    const ForestSplatCells& cells = *splats.Cells;
    ForestCardParameters params{};
    params.Atlas = {cells.Origin.x, cells.Origin.y, cells.Origin.z, cells.CellSize};
    params.Material[0] = color.Material->GetGpuSceneMaterialIndex();
    const DrawBindings::BufferEntry cellBinding{HashStringId("ForestSplatCells"), splats.CellBuffer, 0,
                                                cells.Cells.size() * sizeof(ForestSplatCell)};
    return EmitForestPlacementDraw(context, buffers, splats.Indices, params, &cellBinding, color);
}

} // namespace GameEngine::Engine::Renderer
