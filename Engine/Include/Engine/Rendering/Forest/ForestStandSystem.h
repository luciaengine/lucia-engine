#pragma once

#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
#include "Components/Rendering/ForestStand.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECS.h"
#include "ECS/Systems.h"
#include "Engine/Rendering/Forest/ForestDetailBudget.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Types/FlatMap.h"
#include "Types/FlatSet.h"
#include "Types/ScopedSubscription.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace GameEngine
{
class AssetManager;
namespace Components
{
struct MeshRenderer;
enum class ShadowCasting : uint8;
}
} // namespace GameEngine

namespace GameEngine::Engine::Renderer
{

class RenderServices;
struct ForestPlacementSession;
struct ForestSplatCells;

/// Attaches the packed forest stand of each ForestStand entity, and releases it when the entity, the
/// component or the scene goes. A changed archive or splat file attaches the stand again. Detail Distance and
/// Near Mesh Budget apply to the attached stand, and so does the Shadow Casting of the overview
/// MeshRenderer. A stand that cannot attach logs one warning. Main thread.
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
        /// Last active detail view, when its slot budget omitted a visible detail page.
        std::optional<ForestDetailBudget> DetailBudget;
        Rendering::ViewPurpose DetailView = Rendering::ViewPurpose::EditorScene;
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
    bool HasScheduledWork(ECS::World& world) override;

    // How a stand draws its trees. A change between modes attaches the stand again.
    enum class DetailMode : uint8
    {
        Off,
        Cards,
        CardsAndNearMeshes,
        Invalid
    };

    // What the attached stand was made from. A change to any field attaches the stand again.
    struct StandSource
    {
        GUID Archive;
        GUID TreeModel;
        DetailMode Detail = DetailMode::Off;
        Components::ForestFarRepresentation Far = Components::ForestFarRepresentation::Cards;
        GUID Splats;
        GUID OverviewModel;
        uint32 OverviewMesh = 0;
        bool RendererValid = false;

        bool operator==(const StandSource&) const = default;
    };

    struct Stand
    {
        StandSource Source;
        // Empty when the stand could not attach; the source then holds the warning back until it changes.
        ScopedSubscription Registration;
        // The detail session of an attached stand with trees, else null.
        std::shared_ptr<ForestPlacementSession> Details;
        // The sun-shadow casters of the detail session.
        ScopedSubscription ShadowRegistration;
        // The overview index count that the archive's pages address.
        uint32 OverviewIndexEnd = 0;
        // True while the resident overview mesh has another index count than the archive's pages address.
        bool OverviewMismatch = false;
        // The warning from the last attach, or empty.
        std::string AttachProblem;
        StandStatus Status;
    };

    // Archive and splat GUIDs whose file changed. The file watcher thread adds; the main thread takes.
    struct ChangedArchives
    {
        std::mutex Mutex;
        FlatSet<GUID> Guids;
        bool TracksStands = false;
    };

    // True when a ForestStand or the MeshRenderer of a stand changed, or an entity came or went.
    bool HasStandChanges(ECS::World& world);
    // Attaches the new and changed stands, releases the stands whose entity or component went, and applies
    // Detail Distance and Near Mesh Budget to the kept stands.
    void Reconcile(ECS::World& world);
    // Releases the stands whose archive or splat file changed, so that the next reconcile attaches them again.
    // True when it released a stand.
    bool ReleaseChangedArchives();
    // Reports a stand whose resident overview mesh does not match the pages of its archive.
    void CheckOverviews(ECS::World& world);
    // Sets the session's distance and budget from the component, and its shadow casting and color draw
    // from the overview MeshRenderer.
    void ApplyDetailSettings(Stand& stand, const Components::ForestStand& settings,
                             Components::ShadowCasting shadowCasting) const;
    // Registers the stand's draws and fills its registration, details and attach problem.
    void Attach(ECS::World& world, ECS::EntityHandle entity, const Components::ForestStand& settings, Stand& stand);
    static DetailMode DetailModeOf(const Components::ForestStand& settings);
    // The splat bake of a stand set to Splats, or null with a problem that says why it draws cards.
    std::shared_ptr<const ForestSplatCells> LoadSplats(const StandSource& source, std::string& problem) const;

    RenderServices* m_RenderServices = nullptr;
    AssetManager* m_Assets = nullptr;
    FlatMap<ECS::EntityHandle, Stand> m_Stands;
    uint64 m_WorldId = 0;
    uint64 m_EntityHandleEpoch = 0;
    std::size_t m_StructuralVersion = static_cast<std::size_t>(-1);
    ECS::ChangeGate m_StandGate;
    ECS::ChangeGate m_RendererGate;
    std::shared_ptr<ChangedArchives> m_ChangedArchives = std::make_shared<ChangedArchives>();
    AssetReloadInvalidator m_ArchiveWatch;
    AssetReloadInvalidator m_SplatWatch;
};

} // namespace GameEngine::Engine::Renderer
