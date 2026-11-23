#include "Inspectors/ForestStandInspector.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/ForestStand.h"
#include "Core/Engine.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Engine/Rendering/Forest/ForestStandSystem.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/InspectorNotice.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{

using Stand = Components::ForestStand;
using StandStatus = Engine::Renderer::ForestStandSystem::StandStatus;

// The problem of the stand on `entity`, or empty while it draws all it is set to draw.
std::string FindStandProblem(const ECS::World& world, ECS::EntityHandle entity)
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    const auto* stands = systems ? systems->GetSystem<Engine::Renderer::ForestStandSystem>() : nullptr;
    const StandStatus* status = stands ? stands->FindStatus(world.GetWorldId(), entity) : nullptr;
    return status ? status->Problem : std::string();
}

// The notice of one stand and the problem it shows. The notice is null while there is no problem.
struct StandProblemFollower
{
    EditorUI::InspectorNotice* Notice = nullptr;
    const ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    std::function<void()> RequestRefresh;
    std::string Shown;
    bool RefreshRequested = false;
};

// The stand system sets the problem; the notice follows it. A problem that comes or goes rebuilds
// the inspector.
void FollowStandProblem(StandProblemFollower& follower)
{
    if (follower.RefreshRequested)
        return;
    std::string current = FindStandProblem(*follower.World, follower.Entity);
    if (current == follower.Shown)
        return;
    if (follower.Notice && !current.empty())
    {
        follower.Notice->SetText("This stand " + current);
        follower.Shown = std::move(current);
        return;
    }
    if (!follower.RequestRefresh)
        return;
    follower.RefreshRequested = true;
    follower.RequestRefresh();
}

void AddStandProblemNotice(const InspectorContext& ctx)
{
    StandProblemFollower follower{.World = ctx.World, .Entity = ctx.Entity,
                                  .RequestRefresh = ctx.RequestInspectorRefresh,
                                  .Shown = FindStandProblem(*ctx.World, ctx.Entity)};
    if (!follower.Shown.empty())
        follower.Notice = InspectorUI::AddNotice(ctx.Parent, "This stand " + follower.Shown,
                                                 EditorUI::InspectorNotice::Kind::Warning);
    if (ctx.SimulationRefreshCallbacks)
        ctx.SimulationRefreshCallbacks->push_back([follower]() mutable { FollowStandProblem(follower); });
}

// A drag previews nothing: each changed Detail Distance or Near Mesh Budget attaches the stand again,
// so the value applies on release.
template<typename V>
void KeepDuringDrag(Stand&, V)
{
}

void BuildForestStandInspector(const InspectorContext& ctx)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return;
    const auto* stand = ctx.World->GetComponent<Stand>(ctx.Entity);
    if (!stand)
        return;

    ECS::World* w = ctx.World;
    const ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();

    AddStandProblemNotice(ctx);

    InspectorUI::AddAssetFieldRow(ctx.Parent, "Archive", stand->Archive.ToGuid(), {AssetType::ForestPlacementArchive},
        &registry,
        [w, e, n, undo](const GUID& picked)
        {
            InspectorDrag::CommitComponentWithUndo<Stand>(w, e, n, undo, "Change Forest Stand Archive",
                [picked](Stand& u) { u.Archive.Set(picked); });
        },
        nullptr, "The .geforest file that ForestPlacementCook wrote with this entity's overview mesh.");

    InspectorUI::AddAssetFieldRow(ctx.Parent, "Tree Model", stand->TreeModel.ToGuid(), {AssetType::Model}, &registry,
        [w, e, n, undo](const GUID& picked)
        {
            InspectorDrag::CommitComponentWithUndo<Stand>(w, e, n, undo, "Change Forest Stand Tree Model",
                [picked](Stand& u) { u.TreeModel.Set(picked); });
        },
        nullptr, "The tree model with its impostor setting on. Empty draws the overview only. The trees draw in the "
                 "Game view while it draws, else in the Scene View: in play mode with both views open, the Scene "
                 "View shows the overview only.");

    auto distance = InspectorDrag::MakeComponentInteractiveHandlers<Stand, float>(w, e, n, undo,
        "Change Forest Stand Detail Distance", &KeepDuringDrag<float>,
        [](Stand& u, float v) { u.DetailDistance = std::max(0.0f, v); }, {});
    InspectorDrag::AddFloatRowWithDrag(ctx.Parent, "Detail Distance", stand->DetailDistance,
        std::move(distance.first), std::move(distance.second), Stand{}.DetailDistance,
        "Meters from the camera within which the stand draws trees instead of the overview. 0 draws the overview "
        "only. The value applies when the drag ends.",
        0.0f);

    auto budget = InspectorDrag::MakeComponentInteractiveHandlers<Stand, int>(w, e, n, undo,
        "Change Forest Stand Near Mesh Budget", &KeepDuringDrag<int>,
        [](Stand& u, int v) { u.NearMeshBudget = static_cast<uint32>(std::clamp<int>(v, 0, Stand::kMaxNearMeshBudget)); },
        {});
    IntField* budgetField = InspectorDrag::AddIntRowWithDrag(ctx.Parent, "Near Mesh Budget",
        static_cast<int>(std::min(stand->NearMeshBudget, Stand::kMaxNearMeshBudget)),
        std::move(budget.first), std::move(budget.second), 0,
        "The most trees near the camera that draw their mesh instead of the impostor card, at most 256, or 128 on "
        "the compatibility tier. 0 draws cards only. The value applies when the drag ends.");
    budgetField->SetRange(0, static_cast<int>(Stand::kMaxNearMeshBudget));
}

} // namespace

void RegisterForestStandInspector()
{
    Editor::EditorComponentTraits traits;
    traits.DisplayName = "Forest Stand";
    traits.InspectorCategory = "Rendering";
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Stand>(), std::move(traits));

    InspectorRegistry::Get().RegisterComponentInspector<Stand>(&BuildForestStandInspector);
}

} // namespace GameEngine
