#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderSorting.h"
#include "Engine/Rendering/SortingLayers.h"
#include "InspectorRegistry.h"
#include "UI/Controls/Label.h"

#include <memory>

namespace GameEngine::Editor
{
namespace
{
struct DiagnosticState
{
    std::function<ECS::World*()> GetWorld;
    ECS::EntityHandle Entity;
    Label* Text = nullptr;
    const ECS::World* LastWorld = nullptr;
    uint64 WorldVersion = 0;
    uint64 LayerRevision = 0;
    Rendering::SortingLayers Layers;
};

void RefreshDiagnostic(DiagnosticState& state)
{
    auto* world = state.GetWorld();
    if (!world || !world->IsValid(state.Entity))
        return;
    const uint64 revision = Rendering::SortingLayers::Revision();
    const uint64 version = world->GetGlobalSystemVersion();
    if (state.LastWorld == world && state.WorldVersion == version && state.LayerRevision == revision)
        return;
    if (state.LayerRevision != revision)
        state.Layers = Rendering::SortingLayers::Current();
    state.LastWorld = world;
    state.WorldVersion = version;
    state.LayerRevision = revision;
    const auto sorting = Engine::Renderer::ResolveRenderSorting(*world, state.Entity, state.Layers);
    std::string layerName = "Default";
    for (const auto& layer : state.Layers.Layers)
        if (layer.Order == sorting.Layer)
        {
            layerName = layer.Name;
            break;
        }
    std::string text = "Effective sorting: " + layerName + " / " + std::to_string(sorting.Order);
    if (sorting.Grouped)
    {
        const auto* name = world->GetComponent<Components::Name>(sorting.Group);
        const std::string group = name && !name->View().empty() ? std::string(name->View()) :
            "Entity " + std::to_string(sorting.Group.id);
        text += " / Group: " + group + " / Member order: " + std::to_string(sorting.MemberOrder);
    }
    else
        text += " / No group";
    state.Text->SetText(text);
}
}

void AddRenderSortingDiagnostic(const InspectorContext& context)
{
    if (!context.Parent || !context.World)
        return;
    auto label = std::make_unique<Label>();
    label->AddClass("inspector-field-caption");
    auto state = std::make_shared<DiagnosticState>();
    state->Text = label.get();
    state->Entity = context.Entity;
    state->GetWorld = context.GetWorld ? context.GetWorld : [world = context.World]() { return world; };
    context.Parent->AddChild(std::move(label));
    RefreshDiagnostic(*state);
    if (context.SimulationRefreshCallbacks)
        context.SimulationRefreshCallbacks->push_back([state]() { RefreshDiagnostic(*state); });
}
}
