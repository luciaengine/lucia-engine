#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Rendering/Sorting/SortingLayerSettings.h"
#include "UndoRedo/UndoRedoService.h"

namespace GameEngine::Editor
{
namespace
{
using Sorting = Components::RenderSorting;
void BuildInspector(const InspectorContext& context)
{
    if (!context.Parent || !context.World)
        return;
    const auto* sorting = context.World->GetComponent<Sorting>(context.Entity);
    if (!sorting)
        return;
    if (!HasSortingRenderer(*context.World, context.Entity))
        AddRenderSortingFields(context);
    auto* world = context.World;
    const auto entity = context.Entity;
    auto* notifications = context.ChangeNotifications;
    auto* undo = context.Undo;
    const auto additional = InspectorDrag::GetAdditionalEntities(context);
    InspectorDrag::AddToggleRow(context.Parent, "Group Children", sorting->GroupChildren,
        [world, entity, additional, notifications, undo](bool enabled)
        {
            InspectorDrag::CommitComponentsWithUndo<Sorting>(world, entity, additional, notifications, undo,
                "Change Sorting Group", [enabled](Sorting& value) { value.GroupChildren = enabled; });
        }, "Draw descendants together at this parent's layer, order, and sorting point.");
    InspectorDrag::AddToggleRow(context.Parent, "Sort by Y", sorting->SortByY,
        [world, entity, additional, notifications, undo](bool enabled)
        {
            InspectorDrag::CommitComponentsWithUndo<Sorting>(world, entity, additional, notifications, undo,
                "Change Y Sorting", [enabled](Sorting& value) { value.SortByY = enabled; });
        }, "Draw higher ground points first (+Y up). Enable on sprites and tile layers that overlap. Sorting layer and order take priority.");
    InspectorDrag::AddComponentFloatRowWithDrag<Sorting>(context.Parent, "Y Sort Origin", sorting->SortOriginY,
        world, entity, notifications, undo, "Change Y Sort Origin",
        [](Sorting& value, float origin) { value.SortOriginY = origin; }, 0.0f,
        "Ground point offset along local Y, before scale. Sprites use their entity origin; tiles use each cell's bottom center.",
        additional);
}
}

void RegisterRenderSortingEditor()
{
    RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::MeshRenderer>());
    RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::SkinnedMeshRenderer>());
    InspectorRegistry::Get().RegisterComponentInspector<Sorting>(BuildInspector);
    EditorComponentTraits traits;
    traits.DisplayName = "Render Sorting";
    traits.Category = "Rendering";
    traits.Tooltip = "Authored sorting layers and groups for alpha-blended artwork.";
    EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Sorting>(), std::move(traits));
    RegisterSortingLayerSettings();
    EditorPanelRegistry::Get().RegisterEditorStyleSheet({"editor", "UI/panels/HierarchyLayerControls.css"});
}
}
