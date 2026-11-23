#include "Engine/Rendering/Forest/ForestStandSystem.h"

#include "Assets/AssetManager.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/Forest/ForestPlacementFeature.h"
#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Engine/Rendering/Forest/ForestStandDraws.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
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
};

constexpr ForestStandResidency kCompatibilityResidency{
    .DetailSlots = 128, .TransitionSlots = 4, .ReadsPerFrame = 4, .NearMeshCap = 128};
constexpr ForestStandResidency kDefaultResidency{
    .DetailSlots = 512, .TransitionSlots = 0, .ReadsPerFrame = 12, .NearMeshCap = 256};

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
std::string DescribeRendererProblem(const Components::MeshRenderer* renderer)
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

uint32 CountPlacements(const ForestPlacementRuntime& placements)
{
    const auto pages = placements.Store().Pages();
    return std::accumulate(pages.begin(), pages.end(), uint32{0},
        [](uint32 count, const PageStreaming::PlacementPage& page) { return count + page.Count; });
}

} // namespace

bool ForestStandSystem::StandSource::operator==(const StandSource& other) const
{
    return Settings.Archive == other.Settings.Archive && Settings.TreeModel == other.Settings.TreeModel &&
           Settings.DetailDistance == other.Settings.DetailDistance &&
           Settings.NearMeshBudget == other.Settings.NearMeshBudget && OverviewModel == other.OverviewModel &&
           OverviewMesh == other.OverviewMesh && RendererValid == other.RendererValid;
}

ForestStandSystem::ForestStandSystem(RenderServices* renderServices, AssetManager* assets)
    : m_RenderServices(renderServices), m_Assets(assets)
{
}

ForestStandSystem::~ForestStandSystem() = default;

void ForestStandSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices || !m_Assets)
        return;

    // A new run of entity handles (a scene load clears the world) releases every stand.
    if (world.GetWorldId() != m_WorldId || world.GetEntityHandleEpoch() != m_EntityHandleEpoch)
    {
        m_Stands.Clear();
        m_WorldId = world.GetWorldId();
        m_EntityHandleEpoch = world.GetEntityHandleEpoch();
        m_ResetGeneration = world.GetLifecycleResetGeneration() + 1;
    }
    // A clear or a snapshot restore replaces the chunks: the gates start again.
    if (world.GetLifecycleResetGeneration() != m_ResetGeneration)
    {
        m_ResetGeneration = world.GetLifecycleResetGeneration();
        m_StandGate = {};
        m_RendererGate = {};
        m_StructuralVersion = static_cast<std::size_t>(-1);
    }
    if (HasStandChanges(world))
        Reconcile(world);
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

void ForestStandSystem::Reconcile(ECS::World& world)
{
    std::vector<std::pair<ECS::EntityHandle, Components::ForestStand>> found;
    world.Query<ECS::Read<Components::ForestStand>>().Each(
        [&found](ECS::EntityHandle entity, const Components::ForestStand& stand) { found.emplace_back(entity, stand); });

    FlatMap<ECS::EntityHandle, Stand> stands;
    for (const auto& [entity, settings] : found)
    {
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity);
        StandSource source{.Settings = settings};
        if (renderer)
        {
            source.OverviewModel = renderer->modelAssetGuid.ToGuid();
            source.OverviewMesh = renderer->meshId;
            source.RendererValid = DescribeRendererProblem(renderer).empty();
        }

        if (auto* kept = m_Stands.Find(entity); kept && kept->Source == source)
        {
            stands.InsertOrAssign(entity, std::move(*kept));
            continue;
        }
        std::string problem;
        Stand stand{.Source = source};
        stand.Registration = Attach(world, entity, source, problem);
        if (!problem.empty())
            Logger::Log::Warning("Forest stand {} {}", DescribeEntity(world, entity), problem);
        stands.InsertOrAssign(entity, std::move(stand));
    }
    m_Stands = std::move(stands);
}

ScopedSubscription ForestStandSystem::Attach(ECS::World& world, ECS::EntityHandle entity, const StandSource& source,
                                             std::string& problem)
{
    if (!source.RendererValid)
    {
        problem = DescribeRendererProblem(world.GetComponent<Components::MeshRenderer>(entity));
        return {};
    }
    const auto& settings = source.Settings;
    AssetMetadata archive;
    if (settings.Archive.IsNull() || !m_Assets->GetRegistry().TryGetAssetMetadata(settings.Archive.ToGuid(), archive) ||
        archive.Path.empty())
    {
        problem = "has no archive in the project. Set Archive to the .geforest file that the overview mesh was cooked from.";
        return {};
    }

    const ForestStandResidency& residency = ResidencyFor(m_RenderServices->GetProfile());
    bool wantsTrees = !settings.TreeModel.IsNull();
    if (wantsTrees && !(std::isfinite(settings.DetailDistance) && settings.DetailDistance > 0.0f))
    {
        problem = "draws the overview only: Detail Distance must be more than 0 meters.";
        wantsTrees = false;
    }
    if (wantsTrees && !m_Assets->GetRegistry().IsAssetRegistered(settings.TreeModel.ToGuid()))
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
        desc.KeepCpuCopy = nearBudget != 0;
    }
    std::string error;
    auto placements = ForestPlacementRuntime::Open(m_Assets->ResolveAssetPath(archive.Path), desc, error);
    if (!placements)
    {
        problem = "cannot open its archive '" + PathToUtf8(archive.Path) + "': " + error +
                  ". Cook the archive again with ForestPlacementCook.";
        return {};
    }

    const uint64 worldId = world.GetWorldId();
    const uint64 epoch = world.GetEntityHandleEpoch();
    std::shared_ptr<ForestPlacementSession> trees;
    if (wantsTrees)
        trees = m_RenderServices->EnsureFeature<ForestPlacementFeature>().Attach(
            placements, *m_Assets, worldId, settings.TreeModel.ToGuid(), settings.DetailDistance, nearBudget);
    const uint32 pageCount = static_cast<uint32>(placements->Store().Pages().size());
    const uint32 placementCount = CountPlacements(*placements);
    auto registration = RegisterForestStandMesh(*m_RenderServices, worldId, Rendering::RenderLayerMask::Default(),
        std::move(placements), Rendering::MeshGPUKey{source.OverviewModel, source.OverviewMesh},
        [worldId, epoch, entity, model = source.OverviewModel, mesh = source.OverviewMesh]
        { return ResolveStandMaterial(worldId, epoch, entity, model, mesh); },
        std::move(trees));
    if (!registration)
    {
        problem = "could not register its draws.";
        return {};
    }
    Logger::Log::Info("Forest stand {} attached with {} pages and {} placements", DescribeEntity(world, entity),
                      pageCount, placementCount);
    return registration;
}

} // namespace GameEngine::Engine::Renderer
