#pragma once

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshGPURegistry.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// One order-dependent Blend draw staged for the drain, CPU-built once per view
// per frame. RunGroup is the dense DISCOVERY-order group ordinal (the run key
// map hands these out); the draw slot comes later from the partition.
struct SortedTransparentRecord
{
    uint32_t InstanceIndex = 0; // GPUScene slot (indirection payload)
    uint32_t IndexCount = 0;    // LOD0 draw args from the MeshGPUEntry
    uint32_t FirstIndex = 0;
    int32_t VertexOffset = 0;
    uint32_t RunGroup = 0;
    float ViewDepth = 0.0f; // dot(center - camPos, camForward); larger = farther
};

// Material state shared by one draw. The strings alias material-owned storage
// only while this view's records are gathered.
struct SortedTransparentRunMaterialKey
{
    std::string_view SurfaceShaderPath;
    std::string_view VertexModifierPath;
    std::string_view LightingModel;
    uint64_t MaterialKeywords = 0u;
    uint64_t UserKeywordHash = 0u;
    MaterialBlendState Blend{}; // resolved: authored or the default equation
    bool DepthWrite = false;
    bool DepthTest = true;
    // Fully-procedural vertex path (CUSTOM_VERTEX_SHADER): changes the
    // compiled variant AND clamps vertexFlags to None at compile, so the
    // geometry key alone cannot split it — it must ride this key or two Blend
    // materials differing only here would fuse and draw foreign records
    // through the representative's pipeline (S2 review R2-3).
    bool CustomVertexShader = false;
    bool DoubleSided = false;
    bool IgnoreVertexColor = false;

    // Compatibility binds one material texture set for the entire direct draw.
    // Bindless runs leave this null because each instance fetches its material.
    const Material* BoundMaterial = nullptr;

    bool operator==(const SortedTransparentRunMaterialKey&) const = default;
};

inline SortedTransparentRunMaterialKey MakeSortedTransparentRunMaterialKey(const Material& mat, bool bindMaterialTextures = false)
{
    const MaterialCompileSpec& spec = mat.GetCompileSpec();
    SortedTransparentRunMaterialKey key{};
    key.BoundMaterial = bindMaterialTextures ? &mat : nullptr;
    key.SurfaceShaderPath = spec.surfaceShaderPath;
    key.VertexModifierPath = spec.vertexModifierPath;
    key.LightingModel = spec.lightingModel;
    key.MaterialKeywords = static_cast<uint64_t>(mat.GetVariantKey().materialKeywords);
    key.UserKeywordHash = mat.GetVariantKey().userKeywordHash;
    key.CustomVertexShader = spec.customVertexShader;
    if (const auto& authored = mat.GetBlendState())
        key.Blend = *authored;
    key.DepthWrite = mat.GetDepthWriteOverride().value_or(false);
    key.DepthTest = mat.GetDepthTestOverride().value_or(true);
    key.DoubleSided = mat.IsDoubleSided();
    key.IgnoreVertexColor = mat.IgnoresVertexColor();
    return key;
}

// Geometry-pool identity of a run: one DrawIndexedIndirectCount binds ONE set
// of VB/IB pools, so every record in a run must resolve to the same bucket
// pool handles (meshes may differ — the indirect commands carry per-record
// index ranges into the shared pools). Topology and the mesh-derived vertex
// flags ride the PSO, so they split runs too.
struct SortedTransparentRunGeometryKey
{
    Rendering::MeshGPUEntryBindings Bindings{};
    Rendering::PrimitiveTopology Topology = Rendering::PrimitiveTopology::TriangleList;
    Rendering::VertexAttributeFlags VertexFlags = Rendering::VertexAttributeFlags::None;

    bool operator==(const SortedTransparentRunGeometryKey&) const = default;
};

// Each slot is a contiguous range in the sorted mesh order. A group can occur
// in multiple slots when another material or contributor falls between its draws.
struct SortedTransparentRunPartition
{
    void Clear()
    {
        GroupOfSlot.clear();
        RunStart.clear();
        RunCount.clear();
    }
    std::vector<uint32_t> GroupOfSlot;
    std::vector<uint32_t> RunStart; // exclusive prefix of RunCount, slot order
    std::vector<uint32_t> RunCount;
};

} // namespace GameEngine::Engine::Renderer
