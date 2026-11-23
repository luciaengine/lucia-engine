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
struct ForestStandSystemTestAccess;

/// Attaches a packed forest stand for each entity with a ForestStand component, in the Editor and
/// the Player alike. It attaches when the entity appears or when its ForestStand or MeshRenderer
/// changes, and it releases the stand when the entity or the component goes, and when the world
/// starts a new run of entity handles (a scene load clears the world). A stand that cannot attach
/// logs one warning with the fix and does not stop the scene. Main thread: it registers draws.
class ForestStandSystem : public ECS::ISystem
{
public:
    ForestStandSystem(RenderServices* renderServices, AssetManager* assets);
    ~ForestStandSystem() override;

    const char* GetName() const override { return "ForestStand"; }
    void Update(ECS::World& world, float32 deltaTime) override;
    // Draw registration and the archive open run on the main thread.
    bool RequiresExclusiveUpdate() const override { return true; }

private:
    friend struct ForestStandSystemTestAccess;

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
    uint64 m_ResetGeneration = 0;
    std::size_t m_StructuralVersion = static_cast<std::size_t>(-1);
    ECS::ChangeGate m_StandGate;
    ECS::ChangeGate m_RendererGate;
};

} // namespace GameEngine::Engine::Renderer
