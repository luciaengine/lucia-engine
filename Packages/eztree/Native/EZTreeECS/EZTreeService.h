#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "EZTree/EZTreeGenerator.h"
#include "EZTreeECS/EZTreeVariantMaterials.h"
#include "Types/FlatMap.h"

#include <mutex>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
class RenderServices;
}

namespace EZTreeECS
{

/// One generated tree shared by every tree with equal options: its meshes, its
/// materials and its stats. Trees of one variant draw as instances of one batch.
struct TreeVariant
{
    uint64 Hash = 0;
    uint32 Slot = 0;
    uint32 RefCount = 0;
    GUID MeshKey;
    Rendering::MeshGPUHandle Combined;
    Rendering::MeshGPUHandle Branches;
    Rendering::MeshGPUHandle Leaves;
    Rendering::MeshGPUHandle Trellis;
    TreeVariantMaterials Materials;
    bool MaterialsReady = false;
    Mathematics::BoundingBox Bounds{};
    float ImpostorRadius = 0.0f; // Largest vertex distance from Bounds.center: the impostor's sphere.
    uint32 VertexCount = 0;
    uint32 TriangleCount = 0;
    uint32 BranchCount = 0;
};

/// Owns the tree variants. A tree takes a reference to the variant of its options hash
/// and gives it back when its options change or the tree goes away; the last reference
/// frees the meshes and returns the slot. Thread safe.
class EZTreeService
{
public:
    static EZTreeService& Get();
    static EZTreeService* TryGet();

    void Initialize(Engine::Renderer::RenderServices* renderServices);
    void Shutdown();

    /// The variant of `hash`, generated from `options` when no tree holds it, with one
    /// more reference. Returns a copy: the table can grow under another thread.
    TreeVariant AcquireVariant(uint64 hash, const EZTree::TreeOptions& options);
    void ReleaseVariant(uint64 hash);
    /// The variant of `hash` without a new reference; Hash is 0 when no tree holds it.
    TreeVariant FindVariant(uint64 hash);
    /// Registers the materials of `hash` again at the next Refresh.
    void InvalidateMaterials(uint64 hash);

    /// Registers the materials again for every variant whose wind materials are not
    /// ready, and regenerates every mesh after an in-place device rebuild freed them.
    void Refresh(uint64 deviceRebuildGeneration);


private:
    void Generate(TreeVariant& variant, const EZTree::TreeOptions& options);
    void FreeMeshes(TreeVariant& variant);

    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    std::mutex m_Mutex;
    FlatMap<uint64, TreeVariant> m_Variants;
    // Generation needs the options; a device rebuild regenerates from these.
    FlatMap<uint64, EZTree::TreeOptions> m_Options;
    std::vector<uint32> m_FreeSlots;
    uint32 m_NextSlot = 0;
    uint64 m_DeviceRebuildGeneration = 0;
};

} // namespace EZTreeECS
} // namespace GameEngine
