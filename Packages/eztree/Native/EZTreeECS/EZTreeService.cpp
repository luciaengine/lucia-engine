#include "EZTreeECS/EZTreeService.h"

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace GameEngine::EZTreeECS
{
namespace
{

GUID VariantMeshKey(uint64 hash)
{
    return GUID::Derive(GUID::Null(), "engine/ez-tree/variant-mesh/" + std::to_string(hash));
}

} // namespace

EZTreeService& EZTreeService::Get()
{
    static EZTreeService service;
    return service;
}

EZTreeService* EZTreeService::TryGet()
{
    return &Get();
}

void EZTreeService::Initialize(Engine::Renderer::RenderServices* renderServices)
{
    std::lock_guard lock(m_Mutex);
    m_RenderServices = renderServices;
}

void EZTreeService::Shutdown()
{
    std::lock_guard lock(m_Mutex);
    for (auto& [hash, variant] : m_Variants)
        FreeMeshes(variant);
    m_Variants.Clear();
    m_Options.Clear();
    m_FreeSlots.clear();
    m_NextSlot = 0;
    m_RenderServices = nullptr;
}

TreeVariant EZTreeService::AcquireVariant(uint64 hash, const EZTree::TreeOptions& options)
{
    std::lock_guard lock(m_Mutex);
    if (TreeVariant* existing = m_Variants.Find(hash))
    {
        ++existing->RefCount;
        return *existing;
    }

    TreeVariant variant{};
    variant.Hash = hash;
    if (m_FreeSlots.empty())
    {
        variant.Slot = m_NextSlot++;
    }
    else
    {
        variant.Slot = m_FreeSlots.back();
        m_FreeSlots.pop_back();
    }
    variant.RefCount = 1;
    variant.MeshKey = VariantMeshKey(hash);
    variant.Materials = VariantMaterialGuids(variant.Slot);
    Generate(variant, options);
    m_Options.InsertOrAssign(hash, options);
    return m_Variants.InsertOrAssign(hash, variant);
}

void EZTreeService::ReleaseVariant(uint64 hash)
{
    std::lock_guard lock(m_Mutex);
    TreeVariant* variant = m_Variants.Find(hash);
    if (!variant || --variant->RefCount > 0)
        return;
    FreeMeshes(*variant);
    m_FreeSlots.push_back(variant->Slot);
    m_Variants.Erase(hash);
    m_Options.Erase(hash);
}

TreeVariant EZTreeService::FindVariant(uint64 hash)
{
    std::lock_guard lock(m_Mutex);
    const TreeVariant* variant = m_Variants.Find(hash);
    return variant ? *variant : TreeVariant{};
}

void EZTreeService::InvalidateMaterials(uint64 hash)
{
    std::lock_guard lock(m_Mutex);
    if (TreeVariant* variant = m_Variants.Find(hash))
        variant->MaterialsReady = false;
}

void EZTreeService::Refresh(uint64 deviceRebuildGeneration)
{
    std::lock_guard lock(m_Mutex);
    if (!m_RenderServices)
        return;
    const bool deviceRebuilt = deviceRebuildGeneration != m_DeviceRebuildGeneration;
    m_DeviceRebuildGeneration = deviceRebuildGeneration;
    for (auto& [hash, variant] : m_Variants)
    {
        const EZTree::TreeOptions* options = m_Options.Find(hash);
        if (!options)
            continue;
        // An in-place device rebuild freed every submesh, but the keys still match.
        if (deviceRebuilt && deviceRebuildGeneration != 0)
        {
            Generate(variant, *options);
            continue;
        }
        if (variant.MaterialsReady)
            continue;
        RegisterVariantMaterials(*m_RenderServices, variant.Materials, *options, variant.Bounds);
        variant.MaterialsReady = VariantMaterialsReady(*m_RenderServices, variant.Materials);
    }
}

void EZTreeService::Generate(TreeVariant& variant, const EZTree::TreeOptions& options)
{
    EZTree::Generator generator;
    const EZTree::GeneratedTree tree = generator.Generate(options);
    variant.Bounds = tree.bounds;
    float radiusSquared = 0.0f;
    for (const auto& vertex : tree.combined.Vertices)
    {
        const Mathematics::Vector3 offset = Mathematics::Vector3(vertex.Position[0], vertex.Position[1], vertex.Position[2]) - tree.bounds.center;
        radiusSquared = std::max(radiusSquared, Mathematics::Vector3::Dot(offset, offset));
    }
    variant.ImpostorRadius = std::sqrt(radiusSquared);
    variant.VertexCount = static_cast<uint32>(tree.combined.Vertices.size());
    variant.TriangleCount = static_cast<uint32>(tree.combined.Indices.size() / 3u);
    variant.BranchCount = tree.stats.generatedBranchCount;
    if (!m_RenderServices)
        return;

    auto& meshes = m_RenderServices->GetMeshGPURegistry();
    // The combined mesh keeps its CPU copy: the scene raycast and the software trace read it.
    variant.Combined = meshes.RegisterSubmesh(Rendering::MeshGPUKey{variant.MeshKey, 0u}, tree.combined, true);
    variant.Branches = {};
    variant.Leaves = {};
    variant.Trellis = {};
    if (!tree.branches.Vertices.empty() && !tree.branches.Indices.empty())
        variant.Branches = meshes.RegisterSubmesh(Rendering::MeshGPUKey{variant.MeshKey, 1u}, tree.branches);
    if (!tree.leaves.Vertices.empty() && !tree.leaves.Indices.empty())
        variant.Leaves = meshes.RegisterSubmesh(Rendering::MeshGPUKey{variant.MeshKey, 2u}, tree.leaves);
    if (!tree.trellis.Vertices.empty() && !tree.trellis.Indices.empty())
        variant.Trellis = meshes.RegisterSubmesh(Rendering::MeshGPUKey{variant.MeshKey, 3u}, tree.trellis);

    RegisterVariantMaterials(*m_RenderServices, variant.Materials, options, variant.Bounds);
    variant.MaterialsReady = VariantMaterialsReady(*m_RenderServices, variant.Materials);
}

void EZTreeService::FreeMeshes(TreeVariant& variant)
{
    if (m_RenderServices && !variant.MeshKey.IsNull())
        m_RenderServices->GetMeshGPURegistry().UnregisterModel(variant.MeshKey);
    variant.Combined = {};
    variant.Branches = {};
    variant.Leaves = {};
    variant.Trellis = {};
}

} // namespace GameEngine::EZTreeECS
