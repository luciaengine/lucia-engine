#include "Inspectors/ForestStandInspector.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/ForestStand.h"
#include "Core/Engine.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace GameEngine
{
namespace
{

using Stand = Components::ForestStand;

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
        nullptr, "The tree model with its impostor setting on. Empty draws the overview only.");

    InspectorDrag::AddComponentFloatRowWithDrag<Stand>(ctx.Parent, "Detail Distance", stand->DetailDistance, w, e, n,
        undo, "Change Forest Stand Detail Distance", [](Stand& u, float v) { u.DetailDistance = std::max(0.0f, v); },
        Stand{}.DetailDistance, "Meters from the camera within which the stand draws trees instead of the overview.",
        {}, 0.0f);

    InspectorDrag::AddComponentIntRowWithDrag<Stand>(ctx.Parent, "Near Mesh Budget",
        static_cast<int>(std::min<uint32>(stand->NearMeshBudget, std::numeric_limits<int>::max())), w, e, n, undo,
        "Change Forest Stand Near Mesh Budget",
        [](Stand& u, int v) { u.NearMeshBudget = static_cast<uint32>(std::max(0, v)); }, 0,
        "The most trees near the camera that draw their mesh instead of the impostor card. 0 draws cards only. "
        "The device tier caps it.");
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
