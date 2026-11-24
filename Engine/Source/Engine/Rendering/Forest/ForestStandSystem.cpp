#include "Engine/Rendering/Forest/ForestStandSystem.h"

#include "Assets/AssetManager.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ForestPlacementFeature.h"
#include "ForestPlacementRuntime.h"
#include "ForestPlacementSession.h"
#include "ForestStandDrawProducer.h"
#include "ForestStandDraws.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <mutex>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

// Page residency of one stand for a device tier.
struct ForestStandResidency
{
    uint32 DetailSlots;
    uint32 TransitionSlots;
    uint32 ReadsPerFrame;
    // The cap on ForestStand::NearMeshBudget.
    uint32 NearMeshCap;
    // Pages resident for sun cascades alone.
    uint32 ShadowSlots;
    uint32 ShadowMeshCap;
};

constexpr ForestStandResidency kCompatibilityResidency{
    .DetailSlots = 128, .TransitionSlots = 4, .ReadsPerFrame = 4, .NearMeshCap = 128, .ShadowSlots = 32, .ShadowMeshCap = 32};
constexpr ForestStandResidency kDefaultResidency{.DetailSlots = 512, .TransitionSlots = 0, .ReadsPerFrame = 12,
                                                  .NearMeshCap = Components::ForestStand::kMaxNearMeshBudget,
                                                  .ShadowSlots = 128, .ShadowMeshCap = 64};

const ForestStandResidency& ResidencyFor(const Rendering::RendererProfile& profile)
{
    return profile.IsCompat() ? kCompatibilityResidency : kDefaultResidency;
}

std::string DescribeEntity(const ECS::World& world, ECS::EntityHandle entity)
{
    if (const auto* name = world.GetComponent<Components::Name>(entity); name && !name->View().empty())
        return "'" + std::string(name->View()) + "'";
    return "entity " + std::to_string(entity.index);
}

// The overview mesh problem of a MeshRenderer, or empty when the stand can draw it.
std::string_view DescribeRendererProblem(const Components::MeshRenderer* renderer)
{
    if (!renderer)
        return "has no MeshRenderer. Add a MeshRenderer with the overview mesh that ForestPlacementCook wrote.";
    if (renderer->modelAssetGuid.IsNull())
        return "has a MeshRenderer with no model. Set its model to the overview mesh that ForestPlacementCook wrote.";
    if (renderer->RenderLayers != Rendering::RenderLayerMask::None())
        return "has a MeshRenderer that draws the overview itself. Set its render layers to None; the stand draws it.";
    if (renderer->MeshNameId != 0)
        return "has a MeshRenderer that selects its mesh by name. Select the overview mesh by index.";
    return {};
}

// The material of the overview entity while it is the same entity with the same mesh.
GUID ResolveStandMaterial(uint64 worldId, uint64 epoch, ECS::EntityHandle entity, const GUID& model, uint32 mesh)
{
    const auto* world = ECS::World::FindLiveWorld(worldId);
    if (!world || world->GetEntityHandleEpoch() != epoch || !world->IsValid(entity))
        return {};
    const auto* renderer = world->GetComponent<Components::MeshRenderer>(entity);
    if (!renderer || renderer->modelAssetGuid.ToGuid() != model || renderer->meshId != mesh)
        return {};
    return renderer->materialAssetGuid.ToGuid();
}

// The overview mesh has one quad of two triangles for each archive page.
constexpr uint32 kIndicesPerQuad = 6;

uint32 CountPlacements(const ForestPlacementRuntime& placements)
{
    const auto pages = placements.Store().Pages();
    return std::accumulate(pages.begin(), pages.end(), uint32{0},
        [](uint32 count, const PageStreaming::PlacementPage& page) { return count + page.Count; });
}

} // namespace

ForestStandSystem::ForestStandSystem(RenderServices* renderServices, AssetManager* assets)
    : m_RenderServices(renderServices), m_Assets(assets)
{
    if (!m_Assets)
        return;
    // A cook replaces the archive file; the event comes on the file watcher thread.
    m_ArchiveWatch = AssetReloadInvalidator(m_Assets->GetEventDispatcher(), AssetType::ForestPlacementArchive,
        [changed = m_ChangedArchives](const GUID& archive)
        {
            const std::scoped_lock lock(changed->Mutex);
            changed->Guids.Insert(archive);
        },
        AssetReloadInvalidator::EventSet::ContentEvents);
}

ForestStandSystem::~ForestStandSystem() = default;

const ForestStandSystem::StandStatus* ForestStandSystem::FindStatus(uint64 worldId, ECS::EntityHandle entity) const
{
    if (worldId != m_WorldId)
        return nullptr;
    const Stand* stand = m_Stands.Find(entity);
    return stand ? &stand->Status : nullptr;
}

void ForestStandSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices || !m_Assets)
        return;

    // A new world or a new run of entity handles (a scene load clears the world) releases every
    // stand. The reset structural version makes the next check read every stand again.
    if (world.GetWorldId() != m_WorldId || world.GetEntityHandleEpoch() != m_EntityHandleEpoch)
    {
        m_Stands.Clear();
        m_WorldId = world.GetWorldId();
        m_EntityHandleEpoch = world.GetEntityHandleEpoch();
        m_StructuralVersion = static_cast<std::size_t>(-1);
    }
    const bool standsChanged = HasStandChanges(world);
    if (ReleaseChangedArchives() || standsChanged)
        Reconcile(world);
    CheckOverviews(world);
}

bool ForestStandSystem::ReleaseChangedArchives()
{
    FlatSet<GUID> changed;
    {
        const std::scoped_lock lock(m_ChangedArchives->Mutex);
        std::swap(changed, m_ChangedArchives->Guids);
    }
    if (changed.Empty())
        return false;
    std::vector<ECS::EntityHandle> released;
    for (const auto& [entity, stand] : m_Stands)
        if (changed.Contains(stand.Source.Archive))
            released.push_back(entity);
    for (const ECS::EntityHandle entity : released)
        m_Stands.Erase(entity);
    return !released.empty();
}

void ForestStandSystem::CheckOverviews(ECS::World& world)
{
    if (m_Stands.Empty())
        return;
    const auto& registry = m_RenderServices->GetMeshGPURegistry();
    const Rendering::MeshGPURegistry::TableScope scope(registry);
    for (auto& [entity, stand] : m_Stands)
    {
        if (!stand.Registration)
            continue;
        const auto* mesh = registry.FindByKey({stand.Source.OverviewModel, stand.Source.OverviewMesh});
        // Only a drawable entry has its final index count.
        const bool mismatch = mesh && registry.IsEntryDrawable(*mesh) && mesh->indexCount != stand.OverviewIndexEnd;
        if (mismatch == stand.OverviewMismatch)
            continue;
        stand.OverviewMismatch = mismatch;
        stand.Status.Attached = !mismatch;
        if (!mismatch)
        {
            stand.Status.Problem = stand.AttachProblem;
            continue;
        }
        stand.Status.Problem = std::format("has an overview mesh with {} quads, but its archive has {} pages. Cook "
                                           "the overview mesh again from this archive.",
                                           mesh->indexCount / kIndicesPerQuad, stand.OverviewIndexEnd / kIndicesPerQuad);
        Logger::Log::Warning("Forest stand {} {}", DescribeEntity(world, entity), stand.Status.Problem);
    }
}

void ForestStandSystem::ApplyDetailSettings(Stand& stand, const Components::ForestStand& settings,
                                            Components::ShadowCasting shadowCasting) const
{
    if (!stand.Details)
        return;
    stand.Details->DetailDistance = settings.DetailDistance;
    const auto& residency = ResidencyFor(m_RenderServices->GetProfile());
    stand.Details->NearBudget = std::min(settings.NearMeshBudget, residency.NearMeshCap);
    stand.Details->ShadowMeshBudget = std::min(settings.NearMeshBudget, residency.ShadowMeshCap);
    stand.Details->CastsShadows = shadowCasting != Components::ShadowCasting::Off;
    stand.Details->DrawsColor = shadowCasting != Components::ShadowCasting::ShadowsOnly;
}

bool ForestStandSystem::HasStandChanges(ECS::World& world)
{
    const uint64 probeVersion = world.GetGlobalSystemVersion();
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    bool changed = !ECS::ChangeFilter::Enabled() || structuralVersion != m_StructuralVersion;
    m_StructuralVersion = structuralVersion;

    auto stands = world.Query<ECS::Read<Components::ForestStand>>();
    stands.Changed<Components::ForestStand>(m_StandGate);
    stands.BatchEach([&changed](const Components::ForestStand*, std::size_t) { changed = true; });
    m_StandGate.LastRunVersion = probeVersion;

    auto renderers = world.Query<ECS::Read<Components::ForestStand>, ECS::Read<Components::MeshRenderer>>();
    renderers.Changed<Components::MeshRenderer>(m_RendererGate);
    renderers.BatchEach([&changed](const Components::ForestStand*, const Components::MeshRenderer*, std::size_t)
                        { changed = true; });
    m_RendererGate.LastRunVersion = probeVersion;
    return changed;
}

ForestStandSystem::DetailMode ForestStandSystem::DetailModeOf(const Components::ForestStand& settings)
{
    // A Detail Distance of 0 draws the overview only.
    if (settings.TreeModel.IsNull() || settings.DetailDistance == 0.0f)
        return DetailMode::Off;
    if (!std::isfinite(settings.DetailDistance) || settings.DetailDistance < 0.0f)
        return DetailMode::Invalid;
    return settings.NearMeshBudget == 0 ? DetailMode::Cards : DetailMode::CardsAndNearMeshes;
}

void ForestStandSystem::Reconcile(ECS::World& world)
{
    std::vector<std::pair<ECS::EntityHandle, Components::ForestStand>> found;
    world.Query<ECS::Read<Components::ForestStand>>().Each(
        [&found](ECS::EntityHandle entity, const Components::ForestStand& stand) { found.emplace_back(entity, stand); });

    FlatMap<ECS::EntityHandle, Stand> stands;
    for (const auto& [entity, settings] : found)
    {
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity);
        const auto shadowCasting = renderer ? renderer->shadowCasting : Components::ShadowCasting::On;
        StandSource source{.Archive = settings.Archive.ToGuid(),
                           .TreeModel = settings.TreeModel.ToGuid(),
                           .Detail = DetailModeOf(settings)};
        if (renderer)
        {
            source.OverviewModel = renderer->modelAssetGuid.ToGuid();
            source.OverviewMesh = renderer->meshId;
            source.RendererValid = DescribeRendererProblem(renderer).empty();
        }

        if (auto* kept = m_Stands.Find(entity); kept && kept->Source == source)
        {
            ApplyDetailSettings(*kept, settings, shadowCasting);
            stands.InsertOrAssign(entity, std::move(*kept));
            continue;
        }
        Stand stand{.Source = source};
        Attach(world, entity, settings, stand);
        ApplyDetailSettings(stand, settings, shadowCasting);
        stand.Status.Attached = static_cast<bool>(stand.Registration);
        stand.Status.Problem = stand.AttachProblem;
        if (!stand.AttachProblem.empty())
            Logger::Log::Warning("Forest stand {} {}", DescribeEntity(world, entity), stand.AttachProblem);
        stands.InsertOrAssign(entity, std::move(stand));
    }
    m_Stands = std::move(stands);
}

void ForestStandSystem::Attach(ECS::World& world, ECS::EntityHandle entity, const Components::ForestStand& settings,
                               Stand& stand)
{
    const StandSource& source = stand.Source;
    std::string& problem = stand.AttachProblem;
    if (!source.RendererValid)
    {
        problem = DescribeRendererProblem(world.GetComponent<Components::MeshRenderer>(entity));
        return;
    }
    AssetMetadata archive;
    if (source.Archive.IsNull() || !m_Assets->GetRegistry().TryGetAssetMetadata(source.Archive, archive) ||
        archive.Path.empty())
    {
        problem = "has no archive in the project. Set Archive to the .geforest file that the overview mesh was cooked from.";
        return;
    }

    const ForestStandResidency& residency = ResidencyFor(m_RenderServices->GetProfile());
    bool wantsTrees = source.Detail == DetailMode::Cards || source.Detail == DetailMode::CardsAndNearMeshes;
    if (source.Detail == DetailMode::Invalid)
        problem = "draws the overview only: Detail Distance must be a number of meters, 0 or more.";
    if (wantsTrees && !m_Assets->GetRegistry().IsAssetRegistered(source.TreeModel))
    {
        problem = "draws the overview only: its tree model is not in the project. Set Tree Model to the tree model "
                  "with its impostor setting on, or clear it.";
        wantsTrees = false;
    }

    const uint32 nearBudget = wantsTrees ? std::min(settings.NearMeshBudget, residency.NearMeshCap) : 0;
    ForestPlacementRuntimeDesc desc{};
    if (wantsTrees)
    {
        desc.DetailSlots = residency.DetailSlots;
        desc.TransitionSlots = residency.TransitionSlots;
        desc.ReadsPerFrame = residency.ReadsPerFrame;
        desc.KeepCpuCopy = source.Detail == DetailMode::CardsAndNearMeshes;
        desc.ShadowSlots = residency.ShadowSlots;
    }
    std::string error;
    auto placements = ForestPlacementRuntime::Open(m_Assets->ResolveAssetPath(archive.Path), desc, error);
    if (!placements)
    {
        problem = "cannot open its archive '" + PathToUtf8(archive.Path) + "': " + error +
                  ". Cook the archive again with ForestPlacementCook.";
        return;
    }

    const uint64 worldId = world.GetWorldId();
    const uint64 epoch = world.GetEntityHandleEpoch();
    if (wantsTrees)
        stand.Details = m_RenderServices->EnsureFeature<ForestPlacementFeature>().Attach(
            placements, *m_Assets, worldId, source.TreeModel, settings.DetailDistance, nearBudget);
    const uint32 pageCount = static_cast<uint32>(placements->Store().Pages().size());
    const uint32 placementCount = CountPlacements(*placements);
    stand.OverviewIndexEnd = placements->StandIndexEnd();
    stand.Registration = RegisterForestStandMesh(*m_RenderServices, worldId, Rendering::RenderLayerMask::Default(),
        std::move(placements), Rendering::MeshGPUKey{source.OverviewModel, source.OverviewMesh},
        [worldId, epoch, entity, model = source.OverviewModel, mesh = source.OverviewMesh]
        { return ResolveStandMaterial(worldId, epoch, entity, model, mesh); },
        stand.Details);
    if (!stand.Registration)
    {
        stand.Details.reset();
        problem = "could not register its draws.";
        return;
    }
    if (stand.Details)
        stand.ShadowRegistration = RegisterForestShadowCasters(*m_RenderServices, worldId,
                                                               Rendering::RenderLayerMask::Default(), stand.Details);
    Logger::Log::Info("Forest stand {} attached with {} pages and {} placements", DescribeEntity(world, entity),
                      pageCount, placementCount);
}

} // namespace GameEngine::Engine::Renderer
