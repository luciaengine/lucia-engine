#pragma once

// HLOD runtime state owner (design v0.2 §5.1, runtime integration layer). Bridges
// a loaded scene to its baked .gehlod: on scene load it reads + validates the
// cache, reconciles the content-keyed baked clusters against the entity-keyed live
// scene (HlodReconcile / F6), registers each admitted cluster's merged proxy
// meshes directly (RegisterSubmesh, C7), spawns one runtime-only proxy entity per
// submesh — started EVICTED so members are the resident set — and tags the live
// members. The resulting runtime cluster table is the authority the
// HLODSelectSystem toggles each frame. On unload/reconcile it retires: unregisters
// the proxy meshes (C7, no refcount -> explicit) and destroys the proxy entities.
//
// One HlodRuntime is owned by RenderServices and bound to one World at a time
// (v1: the editor primary world / the packaged Player world).

#include "Assets/HlodMemberGather.h"           // ModelResolver
#include "Engine/Rendering/MeshGPURegistry.h"  // MeshGPUKey
#include "AssetCore/GUID.h"
#include "ECS/ECS.h"                            // EntityHandle
#include "Types/Types.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace GameEngine {

namespace ECS { class World; }
namespace Components { struct MeshRenderer; }
namespace Rendering { class MeshGPURegistry; }

namespace Hlod {

// The authored mesh a member draws: its model asset and submesh (index or name),
// or its GPU mesh handle when it has no model asset. Runtime systems (cluster LOD) switch the
// handle of a model mesh between entries of the same mesh; that is not a
// reassignment, so it does not make a cluster stale.
struct MemberMeshSource {
    GUID Model;
    uint32 Submesh = 0;
    StringId SubmeshName = 0;
    uint64 Handle = 0;

    static MemberMeshSource Of(const Components::MeshRenderer& renderer);
    bool operator==(const MemberMeshSource&) const = default;
};

// One runtime cluster: the live member + proxy entities, the switch inputs, and
// the per-member cached identity for live staleness detection (design §3.4 / C3).
// Members resident when !ProxyActive, proxies resident when ProxyActive — never
// both, never neither, driven by the single ProxyActive boolean (§5.4).
struct RuntimeCluster {
    struct MemberLod
    {
        ECS::EntityHandle Entity;
        GameEngine::Rendering::MeshGPUKey Mesh;
        uint32 BakedLevel = 0;
    };
    struct LodEnvelope
    {
        uint64 MeshRevision = 0;
        uint64 InputRevision = 0;
        float DefaultSseScale = 0.0f;
        float TightSseScale = 0.0f;
        float MaxCenterOffset = 0.0f;
        float RequiredDistanceAtUnitProjection = 0.0f;
        bool Initialized = false;
    };
    float SphereCenter[3] = {0.0f, 0.0f, 0.0f};
    float SphereRadius = 0.0f;
    GameEngine::Rendering::RenderLayerMask RenderLayers = GameEngine::Rendering::RenderLayerMask::All();
    std::vector<GameEngine::ECS::EntityHandle> Members;
    std::vector<GameEngine::ECS::EntityHandle> Proxies;
    // Synthetic mesh keys registered for this cluster's proxy submeshes; retire
    // unregisters each (C7 — no refcount, so this is the only release path).
    std::vector<GameEngine::Rendering::MeshGPUKey> ProxyMeshKeys;
    // Captured at reconcile so a member move (WorldTransform.Version) or a
    // material/mesh reassignment (MeshRenderer) can be detected per-frame and
    // fall the cluster back to members-only with a persistent stale flag (C3).
    std::vector<uint32> MemberTransformVersion;
    std::vector<MemberMeshSource> MemberMeshSources;
    std::vector<GUID>   MemberMaterialGuid;
    std::vector<uint64> MemberMeshContentHashes;
    std::vector<MemberLod> CoarsenedMembers;
    uint32 MaxBakedLevel = 0;
    std::vector<LodEnvelope> MemberLodEnvelopes;
    bool ProxyActive = false; // hysteresis residency state (members resident when false)
    bool Stale = false;       // members-only fallback: never goes proxy-active
};

class HlodRuntime {
public:
    struct MemberLocation
    {
        GameEngine::ECS::EntityHandle Entity;
        uint32 ClusterIndex;
        uint32 MemberIndex;
    };

    std::span<const MemberLocation> FindMemberLocations(GameEngine::ECS::EntityHandle member) const;
    uint64 GetReconcileRevision() const { return m_ReconcileRevision; }

    // Retire any prior cluster set, then reconcile `world` against the sidecar at
    // `cacheFile`. The read validates format + bounds AND the bake's ConfigHash
    // against one recomputed from the live HLODVolume + current cook-semantics
    // versions — a bake from older semantics (e.g. a pre-kLodGeneratorVersion-
    // bump .gehlod) is rejected with a warning and members render directly,
    // never a silent stale proxy. Member drift stays per-cluster (live source
    // hash drives staleness). Registers proxy meshes + spawns proxy entities +
    // tags members. `sceneKey` namespaces the synthetic proxy mesh GUIDs (C7).
    // A missing/invalid/stale cache (or a scene with no enabled HLODVolume)
    // retires and returns 0. Returns the number of runtime clusters built
    // (admitted, with >= 1 live member).
    uint32 ReconcileScene(GameEngine::ECS::World& world,
                          GameEngine::Rendering::MeshGPURegistry& meshReg,
                          const std::filesystem::path& cacheFile,
                          const std::string& sceneKey,
                          const ModelResolver& resolveModel);

    // Destroy proxy entities + unregister proxy meshes + drop member tags. Safe to
    // call with no active clusters.
    void Retire(GameEngine::ECS::World& world, GameEngine::Rendering::MeshGPURegistry& meshReg);

    std::vector<RuntimeCluster>& GetClusters() { return m_Clusters; }
    const std::vector<RuntimeCluster>& GetClusters() const { return m_Clusters; }
    bool HasClusters() const { return !m_Clusters.empty(); }
    uint64 GetBoundWorldId() const { return m_BoundWorldId; }
    bool IsBoundTo(const GameEngine::ECS::World& world) const;
    bool ReloadedGeometryChanged(const GameEngine::ECS::World& world, const RuntimeCluster& cluster,
                                 std::span<const GUID> reloadedModels) const;

private:
    std::vector<RuntimeCluster> m_Clusters;
    std::vector<MemberLocation> m_MemberLocations;
    uint64 m_ReconcileRevision = 0;
    ModelResolver m_ResolveModel;
    uint64 m_BoundWorldId = 0;
    uint64 m_BoundWorldEpoch = 0;
};

} // namespace Hlod
} // namespace GameEngine
