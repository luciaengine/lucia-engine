#include "Terrain/TerrainVolumeNotices.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
// The terrain EFFECT components have no compiled reflection TU (see
// TerrainModifierEffects.h), so no translation unit instantiates World's
// accessors for them. This header is the codebase's user-side instantiation
// path; without it GetComponent<TerrainStampEffect> is an unresolved external.
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainECS/TerrainGpuBake.h"
#include "TerrainECS/TerrainService.h"
#include "Types/StringUtils.h"
#include "UI/Controls/InspectorNotice.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

bool IsGlobalVolume(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* vol = world.GetComponent<Components::TerrainModifierVolume>(entity);
    return vol && vol->Shape == Components::TerrainVolumeShape::Global;
}

} // namespace

void AddGlobalVolumeScopeNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity,
                                bool anySphericalTerrain)
{
    if (!parent || !IsGlobalVolume(world, entity))
        return;

    // Says nothing about strength: Weight still multiplies into every effect
    // (ComputeWeight returns it for a global volume), and its row is right below.
    InspectorUI::AddInfoCard(parent,
                             "Region is every terrain - no edge, no falloff. Moving or resizing "
                             "this volume does not change what it covers.");

    if (anySphericalTerrain)
    {
        parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
            "Global shapes are not baked on spherical (planet) terrains yet - this volume "
            "affects planar / tiled terrains only."));
    }
}

void AddStampInGlobalVolumeNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity)
{
    if (!parent || !IsGlobalVolume(world, entity))
        return;

    const auto* stamp = world.GetComponent<Components::TerrainStampEffect>(entity);
    if (!stamp || !stamp->Enabled)
        return;

    parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "The Stamp effect on this volume is ignored: a stamp projects its mask over a region's "
        "footprint, and a Global volume has none. Pick a Circle or Rectangle shape to use the "
        "stamp, or a Height Offset / Noise effect to change the whole world."));
}

namespace
{

using Reason = TerrainECS::GpuHeightBakeRefusalReason;

// The class defaults.css draws with display: none.
constexpr const char* kHiddenClass = "hidden";

// The last bake report of each tiled terrain in `world`.
std::vector<const TerrainECS::TerrainHeightBakeReport*> HeightBakeReports(ECS::World& world)
{
    std::vector<const TerrainECS::TerrainHeightBakeReport*> reports;
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
        return reports;
    world.Query<ECS::Read<Components::Terrain>>().Each(
        [&](ECS::EntityHandle, const Components::Terrain& terrain)
        {
            const auto* tiled = terrainService->GetTiledTerrainData(
                TerrainECS::TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
            if (tiled)
                reports.push_back(&tiled->HeightBakeReport);
        });
    return reports;
}

// The slow-edit text of a row on `volume`, or empty. A reason on the terrain itself costs every volume
// the same, and no edit to a volume changes it, so it shows no notice.
std::string DescribeCpuHeightBake(const TerrainECS::TerrainHeightBakeReport& report, ECS::EntityHandle volume,
                                  Reason rowCause)
{
    if (report.TerrainRefusal != Reason::None || report.ModifierRefusal.Entity != volume ||
        report.ModifierRefusal.Reason != rowCause)
        return {};
    std::string text;
    switch (rowCause)
    {
    case Reason::PooledBlend:
        text = "Terrain edits show more slowly while this effect uses the Average blend, because the heights "
               "are computed on the CPU. To edit at full speed, pick another blend while you shape the terrain.";
        break;
    case Reason::Stamp:
        text = "Terrain edits show more slowly while this effect is on, because the heights are computed on the "
               "CPU. To edit at full speed, turn the effect off while you shape the terrain.";
        break;
    case Reason::Erosion:
        text = "Terrain edits show more slowly while erosion is on, because the heights are computed on the CPU. "
               "To edit at full speed, set Erosion to 0 while you shape the terrain.";
        break;
    default:
        return {};
    }
    if (report.OtherModifierRefusals == 0)
        return text;
    const std::string others =
        FormatCount(report.OtherModifierRefusals, "other effect also keeps", "other effects also keep");
    return Join(std::array<std::string, 2>{text, others + " the heights on the CPU."}, " ");
}

// One row's notice and the report revisions it shows. The notice stays in the tree and is hidden
// while it has no text, so a later bake shows it at this row.
struct CpuHeightBakeNoticeFollower
{
    std::function<ECS::World*()> GetWorld;
    ECS::World* World = nullptr;
    ECS::EntityHandle Volume{};
    Reason RowCause = Reason::None;
    EditorUI::InspectorNotice* Notice = nullptr;
    // The sum of the reports' revisions at the last read; each revision only increases.
    uint64 ShownRevisions = 0;
};

// Reads the reports again when a revision moved, and shows, rewrites or hides the notice.
void FollowCpuHeightBake(CpuHeightBakeNoticeFollower& follower, bool force)
{
    ECS::World* world = follower.GetWorld ? follower.GetWorld() : follower.World;
    if (!world)
        return;
    const std::vector<const TerrainECS::TerrainHeightBakeReport*> reports = HeightBakeReports(*world);
    uint64 revisions = 0;
    for (const auto* report : reports)
        revisions += report->Revision;
    if (!force && revisions == follower.ShownRevisions)
        return;
    follower.ShownRevisions = revisions;
    std::string text;
    for (const auto* report : reports)
    {
        text = DescribeCpuHeightBake(*report, follower.Volume, follower.RowCause);
        if (!text.empty())
            break;
    }
    follower.Notice->SetText(text);
    if (text.empty())
        follower.Notice->AddClass(kHiddenClass);
    else
        follower.Notice->RemoveClass(kHiddenClass);
}

} // namespace

void AddCpuHeightBakeNotice(const InspectorContext& ctx, Reason rowCause)
{
    if (!ctx.Parent || !ctx.World || rowCause == Reason::None)
        return;
    auto notice = std::make_unique<EditorUI::InspectorNotice>(std::string{}, EditorUI::InspectorNotice::Kind::Information);
    CpuHeightBakeNoticeFollower follower{ctx.GetWorld, ctx.World, ctx.Entity, rowCause, notice.get()};
    ctx.Parent->AddChild(std::move(notice));
    FollowCpuHeightBake(follower, true);
    if (ctx.SimulationRefreshCallbacks)
        ctx.SimulationRefreshCallbacks->push_back([follower]() mutable { FollowCpuHeightBake(follower, false); });
}

} // namespace GameEngine::Editor
