#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Engine/Rendering/SortingLayers.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Rendering/Sorting/SortingLayerSettings.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <charconv>

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
    auto layers = Rendering::SortingLayers::Current();
    std::sort(layers.Layers.begin(), layers.Layers.end(),
        [](const auto& left, const auto& right) { return left.Order < right.Order; });
    std::vector<Dropdown::Option> options;
    int selected = -1;
    for (const auto& layer : layers.Layers)
    {
        if (layer.Id == sorting->Layer)
            selected = static_cast<int>(options.size());
        options.push_back({std::to_string(layer.Id), layer.Name});
    }
    if (selected < 0)
    {
        selected = static_cast<int>(options.size());
        options.push_back({std::to_string(sorting->Layer), "Missing layer (" + std::to_string(sorting->Layer) + ")"});
    }
    auto* dropdown = InspectorUI::AddDropdownRow(context.Parent, "Sorting Layer", options, selected,
        "Named layers are configured in Project Settings > Sorting Layers. Lower priorities draw first.");
    dropdown->SetAutoWidthPopup(true);
    auto* world = context.World;
    const auto entity = context.Entity;
    auto* notifications = context.ChangeNotifications;
    auto* undo = context.Undo;
    const auto additional = InspectorDrag::GetAdditionalEntities(context);
    auto entities = additional;
    entities.push_back(entity);
    dropdown->SetOnValueChanged([world, entities, notifications, undo](const std::string& value)
    {
        int32 layer = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), layer);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
            ApplyRenderSortingLayer(*world, entities, layer, undo, notifications);
    });
    InspectorDrag::AddComponentIntRowWithDrag<Sorting>(context.Parent, "Order in Layer", sorting->OrderInLayer,
        world, entity, notifications, undo, "Change Render Order",
        [](Sorting& value, int32 order) { value.OrderInLayer = order; }, 0,
        "Lower values draw first within a sorting layer. Equal orders use camera depth.", additional);
    InspectorDrag::AddToggleRow(context.Parent, "Group Children", sorting->GroupChildren,
        [world, entity, additional, notifications, undo](bool enabled)
        {
            InspectorDrag::CommitComponentsWithUndo<Sorting>(world, entity, additional, notifications, undo,
                "Change Sorting Group", [enabled](Sorting& value) { value.GroupChildren = enabled; });
        }, "Draw descendants together at this parent's layer, order, and camera depth.");
    AddRenderSortingDiagnostic(context);
    InspectorUI::AddInfoCard(context.Parent,
        "Sorting controls alpha Blend artwork, including mesh renderers. Disable material depth writes for painter order. Opaque and alpha-tested materials use depth.");
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
