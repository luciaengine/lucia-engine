#pragma once

// HLOD runtime switch system (design v0.2 §5, runtime integration layer). Runs in
// the Extraction phase BEFORE RenderExtractionSystem: once per frame, for each
// reconciled cluster in the bound world's HlodRuntime, it computes maximum
// screen coverage over participating views (the ge_SelectLOD metric, so the CPU
// cluster switch and the GPU per-mesh LOD agree at the boundary) and toggles
// residency between the cluster's members and its proxy through
// MeshGPUData.hlodEvicted + GPUScene::RemoveInstances. On any flip it asks
// RenderExtractionSystem (via RenderServices) to run the full lane so the winning
// set is re-added. Members resident XOR proxy resident, in every view slice, by
// single-boolean construction (§5.4).
//
// A member move or a material/mesh reassignment falls the cluster back to
// members-only with a persistent stale flag (§3.4 / C3); a rebake + reload clears
// it. GE_HLOD=0 or a world with no reconciled clusters is a zero-overhead early
// out (the bench neutrality gate).

#include "AssetCore/AssetReloadInvalidator.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "Assets/HlodSelect.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Types/Types.h"

#include <cstdint>
#include <vector>

namespace GameEngine {

namespace Hlod { class HlodRuntime; struct RuntimeCluster; }
namespace Rendering { class GPUScene; }

namespace Engine::Renderer {

class RenderServices;

class HLODSelectSystem : public ECS::ISystem
{
public:
    explicit HLODSelectSystem(RenderServices* renderServices, AssetEventDispatcher* assetEvents = nullptr);

    const char* GetName() const override { return "HLODSelect"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // Proxy-enter: free the members' GPUScene slots (batched) + evict them, and
    // clear the proxies' eviction so the full lane re-adds them.
    void EnterProxy(ECS::World& world, Rendering::GPUScene& scene, Hlod::RuntimeCluster& cluster);
    // Proxy-exit: reverse of EnterProxy.
    void ExitProxy(ECS::World& world, Rendering::GPUScene& scene, Hlod::RuntimeCluster& cluster);
    void MarkEditedMembers(ECS::World& world, Hlod::HlodRuntime& runtime, uint64 entryVersion);
    static bool MembersChanged(const ECS::World& world, const Hlod::RuntimeCluster& cluster);

    RenderServices* m_RenderServices = nullptr;

    // Scratch reused across clusters to avoid per-flip allocation.
    std::vector<uint32_t> m_SlotScratch;
    struct DemandView
    {
        Hlod::ViewLod Projection;
        Rendering::RenderLayerMask Layers;
        uint32 ForceLevel;
        float DefaultSseScale;
        float TightSseScale;
    };
    std::vector<DemandView> m_DemandViews;

    // Visit only changed ECS chunks between structural edits.
    ECS::ChangeGate m_TransformEditGate;
    ECS::ChangeGate m_MeshRendererEditGate;
    ECS::ChangeGate m_LocalBoundsEditGate;
    ECS::ChangeGate m_LodGroupEditGate;
    uint64 m_MemberLodInputRevision = 0;
    // A destroy that empties its chunk stamps no column, so the structural version also opens the scan.
    std::size_t m_StructuralVersion = 0;
    uint64 m_ReconcileRevision = 0;
    uint64 m_ProxyMeshRevision = 0;
    struct ModelReloadInbox;
    SharedPtr<ModelReloadInbox> m_ModelReloadInbox;
    AssetReloadInvalidator m_ModelReloadSubscription;
};

} // namespace Engine::Renderer
} // namespace GameEngine
