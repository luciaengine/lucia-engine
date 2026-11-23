#include "Forest/PlayerForestStands.h"

#include "Assets/AssetManager.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Loading/SceneLoadInstantiated.h"
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
#include <optional>
#include <string_view>
#include <utility>

namespace GameEngine
{
namespace
{
using Engine::Renderer::ForestPlacementFeature;
using Engine::Renderer::ForestPlacementRuntime;
using Engine::Renderer::ForestPlacementRuntimeDesc;
using Engine::Renderer::ForestPlacementSession;
using Engine::Renderer::RenderServices;

struct ForestStandSource
{
    ECS::EntityHandle Entity;
    Components::ModelRef Model;
    uint32 MeshId;
};

std::optional<ForestStandSource> FindStandSource(ECS::World& world, std::string_view entityName)
{
    std::optional<ForestStandSource> source;
    uint32 matches = 0;
    world.Query<ECS::Read<Components::Name>, ECS::Read<Components::MeshRenderer>>().Each(
        [&source, &matches, entityName](ECS::EntityHandle entity, const Components::Name& name,
                                       const Components::MeshRenderer& mesh)
        {
            if (name.View() != entityName)
                return;
            ++matches;
            if (!mesh.modelAssetGuid.IsNull() && mesh.RenderLayers == Rendering::RenderLayerMask::None() &&
                mesh.MeshNameId == 0)
                source = ForestStandSource{entity, mesh.modelAssetGuid, mesh.meshId};
        });
    if (matches != 1)
        return std::nullopt;
    return source;
}

GUID ResolveStandMaterial(uint64 worldId, uint64 epoch, const ForestStandSource& source)
{
    const auto* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world || world->GetWorldId() != worldId || world->GetEntityHandleEpoch() != epoch ||
        !world->IsValid(source.Entity))
        return {};
    const auto* mesh = world->GetComponent<Components::MeshRenderer>(source.Entity);
    if (!mesh || mesh->modelAssetGuid != source.Model || mesh->meshId != source.MeshId)
        return {};
    return mesh->materialAssetGuid.ToGuid();
}

ScopedSubscription AttachStand(SceneLoadInstantiated& scene, RenderServices& services, const ForestStandConfig& config)
{
    const auto source = FindStandSource(scene.World(), config.Entity);
    if (!source)
    {
        scene.Fail("Forest stand entity '" + config.Entity + "' must name one stand with a mesh asset and no render layers");
        return {};
    }

    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto archive = assets.ResolveAssetPath(PathFromUtf8(config.Archive));
    constexpr uint32 kConstrainedSlots = 128;
    constexpr uint32 kDefaultSlots = 512;
    constexpr uint32 kConstrainedReads = 4;
    constexpr uint32 kDefaultReads = 12;
    constexpr uint32 kConstrainedNearMeshes = 128;
    constexpr uint32 kDefaultNearMeshes = 256;
    const bool constrained = services.GetProfile().IsCompat();
    const bool wantsDetails = !config.ImpostorModel.empty() && std::isfinite(config.DetailDistance) &&
                              config.DetailDistance > 0.0f;
    const GUID model = wantsDetails ? assets.ResolveAssetGuid(PathFromUtf8(config.ImpostorModel)) : GUID{};
    if (wantsDetails && model.IsNull())
    {
        scene.Fail("Forest stand impostor model '" + config.ImpostorModel + "' could not be resolved");
        return {};
    }

    const uint32 nearBudget = wantsDetails
        ? std::min(config.NearMeshBudget, constrained ? kConstrainedNearMeshes : kDefaultNearMeshes) : 0;
    ForestPlacementRuntimeDesc desc{};
    if (wantsDetails)
    {
        desc.DetailSlots = constrained ? kConstrainedSlots : kDefaultSlots;
        desc.TransitionSlots = constrained ? kConstrainedReads : 0;
        desc.ReadsPerFrame = constrained ? kConstrainedReads : kDefaultReads;
        desc.KeepCpuCopy = nearBudget != 0;
    }
    std::string error;
    auto placements = ForestPlacementRuntime::Open(archive, desc, error);
    if (!placements)
    {
        scene.Fail("Forest stand archive '" + config.Archive + "': " + error);
        return {};
    }

    const auto worldId = scene.World().GetWorldId();
    const auto epoch = scene.World().GetEntityHandleEpoch();
    std::shared_ptr<ForestPlacementSession> details;
    if (wantsDetails)
        details = services.EnsureFeature<ForestPlacementFeature>().Attach(
            placements, assets, worldId, model, config.DetailDistance, nearBudget);
    auto registration = Engine::Renderer::RegisterForestStandMesh(services, worldId, Rendering::RenderLayerMask::Default(),
        placements, Rendering::MeshGPUKey{source->Model.ToGuid(), source->MeshId},
        [worldId, epoch, stand = *source] { return ResolveStandMaterial(worldId, epoch, stand); }, std::move(details));
    if (!registration)
    {
        scene.Fail("Could not register forest stand entity '" + config.Entity + "'");
        return {};
    }
    const auto pages = placements->Store().Pages();
    const uint32 placementCount = std::accumulate(pages.begin(), pages.end(), uint32{0},
        [](uint32 count, const PageStreaming::PlacementPage& page) { return count + page.Count; });
    LOG_INFO("Player: forest stand attached to '{}' with {} pages and {} placements",
             config.Entity, pages.size(), placementCount);
    return registration;
}
} // namespace

bool PlayerForestStands::Attach(SceneLoadInstantiated& scene, std::span<const ForestStandConfig> configs)
{
    Clear();
    auto* services = EngineCore::GetInstance().GetRenderServices();
    if (configs.empty() || !services)
        return true;

    std::vector<ScopedSubscription> registrations;
    registrations.reserve(configs.size());
    for (const auto& config : configs)
    {
        if (std::ranges::count(configs, config.Entity, &ForestStandConfig::Entity) != 1)
        {
            scene.Fail("Forest stand entity '" + config.Entity + "' is configured more than once");
            return false;
        }
        auto registration = AttachStand(scene, *services, config);
        if (!registration)
            return false;
        registrations.push_back(std::move(registration));
    }
    m_Registrations = std::move(registrations);
    return true;
}

void PlayerForestStands::Clear()
{
    m_Registrations.clear();
}
} // namespace GameEngine
