#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/Pipeline/Nodes/ForwardShaderSpecialization.h"
#include "Engine/Rendering/TextureService.h"
#include "Platform/Environment.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/ReverseZ.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "RenderServicesDetail.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace GameEngine::Engine::Renderer
{

inline bool IndexedDepthReuseEnabled(const Rendering::IDevice& device)
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_INDEXED_DEPTH_REUSE", true);
    if (!enabled)
        return false;

    const auto api = device.GetAPI();
    if (api == Rendering::GraphicsAPI::Vulkan)
        return true;
    if (api != Rendering::GraphicsAPI::Metal || !device.GetCapabilities().supportsIndexedDepthOwnerReplay)
        return false;

    static const bool metalReplayEnabled = Platform::EnvironmentSwitchEnabled("GE_INDEXED_DEPTH_OWNER_REPLAY", false);
    return metalReplayEnabled;
}

inline bool LogIndexedDepthReplayDiagnostics()
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_INDEXED_DEPTH_REUSE_DIAGNOSTICS", false);
    return enabled;
}

inline const ResolvedPassResources::BufferEntry* FindReplayPassBuffer(
    const PassBindingContext& pass, StringId name)
{
    const auto& buffers = pass.PassResources.Buffers;
    const auto entry = std::find_if(buffers.begin(), buffers.end(), [&](const auto& buffer) {
        return buffer.Name == name;
    });
    return entry == buffers.end() ? nullptr : &*entry;
}

inline MaterialSystem::PackedSSBO BoundMaterialParams(const PassBindingContext& pass)
{
    const auto* entry = FindReplayPassBuffer(pass, HashStringId("MaterialParams"));
    constexpr auto maxSize = std::numeric_limits<size_t>::max();
    if (!entry || entry->Offset > maxSize || entry->Range > maxSize)
        return {};
    return {entry->Buffer, static_cast<size_t>(entry->Offset), static_cast<size_t>(entry->Range)};
}

// A donor override can target a constant absent from its shader. Reject it
// before using an owner shader where that constant can change coverage.
inline bool BindIndexedOwnerMaterialRow(Rendering::GraphicsPipelineDesc& pipeline,
                                        const Rendering::ShaderMeta& shader,
                                        std::optional<uint32_t> certifiedRow)
{
    if (std::none_of(shader.SpecConstants.begin(), shader.SpecConstants.end(), [](const auto& constant) {
            return constant.Id == kForwardMaterialRowConstant;
        }))
        return true;
    if (pipeline.Specialization && pipeline.Specialization->GetEntries().contains(kForwardMaterialRowConstant))
        return false;
    if (certifiedRow)
    {
        if (!pipeline.Specialization)
            pipeline.Specialization.emplace();
        pipeline.Specialization->AddConstant(kForwardMaterialRowConstant, *certifiedRow);
    }
    return true;
}

inline std::optional<bool> IndexedDepthHexSpecialization(const Rendering::GraphicsPipelineDesc& pipeline)
{
    // The admitted SPIR-V stock mask declares SpecId 302 with a true default.
    if (!pipeline.Specialization)
        return true;
    const auto& specialization = *pipeline.Specialization;
    const auto& entries = specialization.GetEntries();
    const auto entry = entries.find(kForwardHexTilingConstant);
    if (entry == entries.end())
        return true;
    const auto& value = entry->second;
    if (value.Size != sizeof(uint32_t) || value.Offset > specialization.GetDataSize() ||
        value.Size > specialization.GetDataSize() - value.Offset)
        return std::nullopt;
    uint32_t enabled;
    std::memcpy(&enabled, specialization.GetDataPtr() + value.Offset, sizeof(enabled));
    if (enabled > 1)
        return std::nullopt;
    return enabled != 0;
}

inline bool MatchesIndexedDepthHexTiling(const Rendering::GraphicsPipelineDesc& depth,
                                         const Rendering::GraphicsPipelineDesc& color,
                                         std::span<const uint8_t> currentMaterialRow)
{
    const auto depthHex = IndexedDepthHexSpecialization(depth);
    const auto colorHex = IndexedDepthHexSpecialization(color);
    if (!depthHex || !colorHex)
        return false;
    if (depthHex == colorHex)
        return true;
    // The shaders use the raw Mat.uParams0.z lane. The reflected property
    // name map can be rebuilt when the surface's declarations change.
    const auto* lane = Rendering::FindLegacyMaterialLane("hexTiling");
    if (!lane)
        return false;
    const auto offset = Rendering::LegacyLaneByteOffset(*lane);
    if (currentMaterialRow.size() < offset + sizeof(float))
        return false;
    float hexTiling;
    std::memcpy(&hexTiling, currentMaterialRow.data() + offset, sizeof(hexTiling));
    return hexTiling < 0.5f;
}

inline bool MatchesIndexedDepthRaster(const Rendering::GraphicsPipelineDesc& depth,
                                      const Rendering::GraphicsPipelineDesc& color, bool vertexAlpha)
{
    const auto supportsReplay = [](const Rendering::GraphicsPipelineDesc& pipeline, bool writesDepth) {
        const auto& raster = pipeline.Rasterization;
        const auto& state = pipeline.DepthStencil;
        return pipeline.Kind == Rendering::GraphicsPipelineKind::VertexFragment &&
               pipeline.Topology == Rendering::PrimitiveTopology::TriangleList &&
               raster.polygonMode == Rendering::PolygonMode::Fill &&
               !raster.depthClampEnable && !raster.depthBiasEnable && !raster.rasterizerDiscardEnable &&
               state.depthTestEnable && state.depthWriteEnable == writesDepth &&
               state.depthCompareOp == Rendering::ReverseZ::kNearerOrEqual &&
               !state.depthBoundsTestEnable && !state.stencilTestEnable &&
               !pipeline.ColorBlend.alphaToCoverageEnable &&
               !pipeline.ColorBlend.logicOpEnable &&
               std::none_of(pipeline.ColorBlend.attachments.begin(), pipeline.ColorBlend.attachments.end(),
                            [](const auto& attachment) { return attachment.blendEnable; });
    };
    if (!supportsReplay(depth, true) || !supportsReplay(color, false) ||
        depth.Rasterization.frontFace != color.Rasterization.frontFace ||
        depth.Rasterization.cullMode != color.Rasterization.cullMode)
        return false;
    // The admitted rigid stock shaders use position, UV0 and optional
    // vertex alpha for coverage. The depth layout deliberately omits extra
    // shading streams such as tangent. Compare the fetch for each input
    // coverage actually reads rather than requiring identical layouts.
    const auto matchesInput = [&](uint32_t location) {
        const auto attributeAt = [location](const auto& attributes) {
            return std::find_if(attributes.begin(), attributes.end(),
                                [location](const auto& attribute) { return attribute.location == location; });
        };
        const auto left = attributeAt(depth.VertexAttributes);
        const auto right = attributeAt(color.VertexAttributes);
        if (left == depth.VertexAttributes.end() || right == color.VertexAttributes.end() ||
            left->binding != right->binding || left->offset != right->offset || left->format != right->format)
            return false;

        const auto bindingAt = [slot = left->binding](const auto& bindings) {
            return std::find_if(bindings.begin(), bindings.end(),
                                [slot](const auto& binding) { return binding.binding == slot; });
        };
        const auto depthBinding = bindingAt(depth.VertexBindings);
        const auto colorBinding = bindingAt(color.VertexBindings);
        return depthBinding != depth.VertexBindings.end() && colorBinding != color.VertexBindings.end() &&
               depthBinding->stride == colorBinding->stride && depthBinding->inputRate == colorBinding->inputRate;
    };
    // Fixed locations in Adapters/adapter_vertex.glsl.
    constexpr uint32_t position = 0;
    constexpr uint32_t uv0 = 2;
    constexpr uint32_t colorInput = 4;
    return matchesInput(position) && matchesInput(uv0) && (!vertexAlpha || matchesInput(colorInput));
}

inline bool MatchesIndexedOwnerMeshVertexLayout(
    const Rendering::GraphicsPipelineDesc& pipeline, Rendering::VertexAttributeFlags flags)
{
    using Rendering::VertexAttributeFlags;
    if (!Rendering::HasFlag(flags, VertexAttributeFlags::StandardMesh))
        return false;

    Rendering::GraphicsPipelineDesc expected;
    const auto consumed = flags & (VertexAttributeFlags::StandardMesh | VertexAttributeFlags::HasColor);
    Rendering::BuildVertexLayoutFromFlags(consumed, expected);

    for (const auto& binding : expected.VertexBindings)
    {
        const auto found = std::find_if(pipeline.VertexBindings.begin(), pipeline.VertexBindings.end(),
            [&binding](const auto& candidate) { return candidate.binding == binding.binding; });
        if (found == pipeline.VertexBindings.end() || found->stride != binding.stride ||
            found->inputRate != binding.inputRate)
            return false;
    }
    for (const auto& attribute : expected.VertexAttributes)
    {
        const auto found = std::find_if(pipeline.VertexAttributes.begin(), pipeline.VertexAttributes.end(),
            [&attribute](const auto& candidate) { return candidate.location == attribute.location; });
        if (found == pipeline.VertexAttributes.end() || found->binding != attribute.binding ||
            found->offset != attribute.offset || found->format != attribute.format)
            return false;
    }
    return true;
}

inline bool MatchesIndexedOwnerMeshFragmentInputs(
    const Rendering::StageMeta& fragment, const Rendering::StageMeta& mesh)
{
    for (const auto& input : fragment.Inputs)
    {
        if (input.Builtin || input.Component != 0u || !input.Type.ArrayDims.empty())
            return false;
        const auto output = std::find_if(mesh.Outputs.begin(), mesh.Outputs.end(),
            [&input](const auto& candidate) {
                return candidate.Location == input.Location && candidate.Component == input.Component;
            });
        if (output == mesh.Outputs.end() || output->Type.ArrayDims.size() != 1u ||
            output->Type.Kind != input.Type.Kind || output->Type.Base != input.Type.Base ||
            output->Type.Rows != input.Type.Rows || output->Type.Cols != input.Type.Cols ||
            output->Type.VecSize != input.Type.VecSize)
            return false;
    }
    return true;
}

inline std::optional<bool> StockMaskVertexAlpha(const Rendering::ShaderMeta* meta,
                                                    bool depthOnly, bool crossfading)
{
    if (!meta)
        return std::nullopt;
    const auto& defines = meta->RequiredDefines;
    const auto contains = [&defines](const char* define) {
        return std::find(defines.begin(), defines.end(), define) != defines.end();
    };
    // A cold fade request can serve the regular pipeline. Match the served variant.
    if (!contains(Rendering::kStockMaskCoverageDefine) ||
        contains("GE_DEPTH_ONLY_FRAGMENT") != depthOnly ||
        contains("GE_LOD_CROSSFADE") != crossfading)
        return std::nullopt;
    return contains("HAS_COLOR");
}

inline bool HasBoundIndexedMaskSets(const Rendering::ShaderMeta& meta, const PreparedDrawBindings& bindings)
{
    return !meta.Sets.empty() && std::all_of(meta.Sets.begin(), meta.Sets.end(), [&](const auto& set) {
        return set.Bindings.empty() ||
               (set.Set < bindings.Sets.size() && bindings.Sets[set.Set].IsValid());
    });
}

inline bool HasPublishedReplayAlbedo(const Material& material, const TextureService& textures,
                                    const PreparedDrawBindings& bindings,
                                    const Rendering::RenderGraph::RGPrepareContext& context)
{
    constexpr auto kAlbedo = HashStringId("albedoMap");
    constexpr auto kAlbedoSlot = TextureSlot::kAlbedo;
    const auto globalTextures = textures.BindlessTextureSet();
    const auto albedo = material.GetTexture(kAlbedo);
    return globalTextures.IsValid() && bindings.Sets[1] == globalTextures &&
           !material.IsTextureAwaited(static_cast<uint32_t>(kAlbedoSlot)) &&
           textures.HasPublishedBindlessSampler(material.GetPackedSamplerIndices() & 0xfu) &&
           textures.MatchesPublishedAssetTexture(
               material.GetGuid(), kAlbedo, albedo, material.GetBindlessTextureIndex(kAlbedoSlot)) &&
           !context.HasDeclaredTextureWrite(albedo);
}

inline bool HasDefaultWhiteSurfaceSlots(const Material& material, uint32_t whiteIndex)
{
    if (whiteIndex == 0u || material.GetBindlessTextureIndex(TextureSlot::kAO) != whiteIndex ||
        material.GetBindlessTextureIndex(TextureSlot::kEmissive) != whiteIndex)
        return false;

    const uint32_t samplers = material.GetPackedSamplerIndices();
    constexpr auto kWhiteSampler = static_cast<uint32_t>(Rendering::SamplerPreset::LinearRepeat);
    const auto usesRepeat = [samplers](TextureSlot slot) {
        return ((samplers >> (static_cast<uint32_t>(slot) * 4u)) & 0xfu) == kWhiteSampler;
    };
    return usesRepeat(TextureSlot::kAO) && usesRepeat(TextureSlot::kEmissive);
}

inline bool CanSpecializeWhiteMaps(const Material& material, const TextureService& textures,
                                   const Rendering::GraphicsPipelineDesc& color)
{
    // The caller has verified that the bound buffer holds this material revision.
    if (!HasDefaultWhiteSurfaceSlots(material, textures.DefaultWhiteBindlessIndex()) ||
        !textures.HasPublishedBindlessSampler(static_cast<uint32_t>(Rendering::SamplerPreset::LinearRepeat)))
        return false;

    const auto hexSpec = IndexedDepthHexSpecialization(color);
    if (hexSpec == false)
        return true;
    const auto* hexLane = Rendering::FindLegacyMaterialLane("hexTiling");
    if (!hexSpec || !hexLane)
        return false;
    const auto offset = Rendering::LegacyLaneByteOffset(*hexLane);
    if (material.GetCacheSize() < offset + sizeof(float))
        return false;
    float hexTiling;
    std::memcpy(&hexTiling, material.GetCacheData() + offset, sizeof(hexTiling));
    return hexTiling < 0.5f;
}

// Identity of the indexed GPU draw source. The caller must validate same-frame
// publication, depth attachment lineage, and raster and material coverage.
struct IndexedDepthDraw
{
    Rendering::BufferHandle Commands;
    Rendering::BufferHandle Count;
    size_t CommandOffset;
    size_t CountOffset;
    uint32_t MaxDrawCount;
    uint32_t Stride;
    Rendering::MeshGPUEntryBindings Geometry;
    uint64_t IndirectionAddress;
    uint64_t InstancesAddress;
    uint32_t MaterialOverride;

    bool operator==(const IndexedDepthDraw&) const = default;
};

inline std::optional<IndexedDepthDraw> GetIndexedDepthDraw(
    const DrawCommand& command, const Rendering::MeshGPUEntryBindings& geometry)
{
    constexpr uint32_t kIndexedRecordBytes = 5 * sizeof(uint32_t);
    const auto constants = command.Bindings.PushConstants;
    if (!command.UseIndirect || !command.SubDraws.empty() ||
        !command.IndirectCommandBuffer.IsValid() || !command.IndirectCountBuffer.IsValid() ||
        command.IndirectMaxDrawCount == 0 || command.IndirectStride != kIndexedRecordBytes ||
        command.IndirectCommandOffset % sizeof(uint32_t) != 0 ||
        command.IndirectCountOffset % sizeof(uint32_t) != 0 ||
        !geometry.coreVB.IsValid() || !geometry.indexBuffer.IsValid() ||
        !command.Bindings.Buffers.empty() || !command.Bindings.Textures.empty() ||
        constants.size() != sizeof(ComposedPCInstanced))
        return std::nullopt;

    ComposedPCInstanced instances{};
    std::memcpy(&instances, constants.data(), sizeof(instances));
    if (instances.indirectionAddr == 0 || instances.gpuInstancesAddr == 0)
        return std::nullopt;

    return IndexedDepthDraw{
        command.IndirectCommandBuffer, command.IndirectCountBuffer,
        command.IndirectCommandOffset, command.IndirectCountOffset,
        command.IndirectMaxDrawCount, command.IndirectStride, geometry,
        instances.indirectionAddr, instances.gpuInstancesAddr, instances.materialOverride
    };
}

} // namespace GameEngine::Engine::Renderer
