#pragma once

#include "AssetCore/GUID.h"
#include "Components/Rendering/ForestStand.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECS.h"
#include "ECS/Systems.h"
#include "Types/FlatMap.h"
#include "Types/ScopedSubscription.h"

#include <cstddef>
#include <string>

namespace GameEngine
{
class AssetManager;
namespace Components
{
struct MeshRenderer;
}
} // namespace GameEngine

namespace GameEngine::Engine::Renderer
{

class RenderServices;

/// Attaches the packed forest stand of each ForestStand entity, and releases it when the entity, the
/// component or the scene goes. A stand that cannot attach logs one warning. Main thread.
class ForestStandSystem : public ECS::ISystem
{
public:
    /// What the stand of one entity draws.
    struct StandStatus
    {
        /// True when the stand draws at least its overview.
        bool Attached = false;
        /// The warning the stand logged, after the entity name, or empty. For example "has no MeshRenderer. ..."
        std::string Problem;
    };

    ForestStandSystem(RenderServices* renderServices, AssetManager* assets);
    ~ForestStandSystem() override;

    const char* GetName() const override { return "ForestStand"; }
    void Update(ECS::World& world, float32 deltaTime) override;
    // Draw registration and the archive open run on the main thread.
    bool RequiresExclusiveUpdate() const override { return true; }

    /// The stand of `entity` in world `worldId` as of the last update, or null when the system holds no
    /// stand for it.
    const StandStatus* FindStatus(uint64 worldId, ECS::EntityHandle entity) const;

private:
    // What the attached stand was made from. A change to any field attaches the stand again.
    struct StandSource
    {
        Components::ForestStand Settings;
        GUID OverviewModel;
        uint32 OverviewMesh = 0;
        bool RendererValid = false;

        bool operator==(const StandSource& other) const;
    };

    struct Stand
    {
        StandSource Source;
        // Empty when the stand could not attach; the source then holds the warning back until it changes.
        ScopedSubscription Registration;
        StandStatus Status;
    };

    // True when a ForestStand or the MeshRenderer of a stand changed, or an entity came or went.
    bool HasStandChanges(ECS::World& world);
    // Attaches the new and changed stands and releases the stands whose entity or component went.
    void Reconcile(ECS::World& world);
    // Registers the stand's draws. Returns an empty subscription with `problem` set when it cannot.
    ScopedSubscription Attach(ECS::World& world, ECS::EntityHandle entity, const StandSource& source,
                              std::string& problem);

    RenderServices* m_RenderServices = nullptr;
    AssetManager* m_Assets = nullptr;
    FlatMap<ECS::EntityHandle, Stand> m_Stands;
    uint64 m_WorldId = 0;
    uint64 m_EntityHandleEpoch = 0;
    std::size_t m_StructuralVersion = static_cast<std::size_t>(-1);
    ECS::ChangeGate m_StandGate;
    ECS::ChangeGate m_RendererGate;
};

} // namespace GameEngine::Engine::Renderer
