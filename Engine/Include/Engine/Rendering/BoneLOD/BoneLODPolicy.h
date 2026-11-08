#pragma once

#include "AssetCore/GUID.h"
#include "Assets/HlodSelect.h"
#include "Components/Animation/BoneLOD.h"
#include "ECS/ECS.h"
#include "Types/Types.h"

namespace GameEngine
{
class AssetManager;
class ModelBoneLOD;
namespace ECS { class World; }
namespace Components
{
struct SkeletonRef;
struct LocalBounds;
struct WorldTransform;
struct MeshRenderer;
struct AnimatedNodeRef;
struct HumanoidRetargeterComponent;
struct Animator;
struct LODGroup;
}

namespace Engine::Renderer
{
class ViewRegistry;

/// Selects one immutable model plan from every consumer's current view demand.
/// Update and Find are serialized; views and model metadata stay stable during Update.
class BoneLODPolicy
{
public:
    struct Selection
    {
        SharedPtr<const ModelBoneLOD> Data;
        uint64 RuntimeGeneration = 0u;
        bool Reduced = false;
    };

    void Update(ECS::World& world, const ViewRegistry* views, const AssetManager* assets, float32 globalBias);
    /// Borrowed storage remains valid until the next Update.
    const Selection* Find(uint32 runtimeId) const;

private:
    struct Consumer
    {
        ECS::EntityHandle Entity;
        const Components::SkeletonRef& Skeleton;
        const Components::LocalBounds* Bounds;
        const Components::WorldTransform* Transform;
        const Components::MeshRenderer* Mesh;
        const Components::AnimatedNodeRef* Node;
        const Components::HumanoidRetargeterComponent* Retargeter;
        const Components::Animator* Animator;
        const Components::LODGroup* LOD;
    };

    struct RuntimeState
    {
        Selection Selected;
        uint64 SeenEpoch = 0u;
        float32 Demand = 0.0f;
        bool HasDemand = false;
    };

    struct FrameView
    {
        Hlod::ViewLod View;
        uint32 LayerMask = 0u;
        bool Orthographic = false;
        bool Valid = false;
    };

    void GatherViews(const ViewRegistry& views, uint64 worldId, float32 globalBias);
    void Accumulate(ECS::World& world, const AssetManager& assets, const Consumer& consumer);
    SharedPtr<const ModelBoneLOD> ResolveModel(const AssetManager& assets, const GUID& guid);

    FastHashMap<ECS::EntityHandle, Components::BoneLOD, ECS::EntityHandleHash> m_Settings;
    FastHashMap<uint32, RuntimeState> m_Runtimes;
    FastHashSet<uint32> m_ForceFull;
    FastHashMap<GUID, SharedPtr<const ModelBoneLOD>> m_Models;
    std::vector<FrameView> m_Views;
    uint64 m_WorldId = 0u;
    uint64 m_WorldGeneration = 0u;
    uint64 m_Epoch = 0u;
};

} // namespace Engine::Renderer
} // namespace GameEngine
