#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Rendering/RenderSorting.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/SortingLayers.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>

namespace GameEngine::Editor
{
namespace
{
using Sorting = Components::RenderSorting;

struct SortingFieldsState
{
    std::function<ECS::World*()> ResolveWorld;
    std::vector<ECS::EntityHandle> Entities;
    Dropdown* Layer = nullptr;
    IntField* Order = nullptr;
    UndoRedoService* Undo = nullptr;
    EditorChangeNotifications* Notifications = nullptr;
    std::vector<Dropdown::Option> Options;
    std::optional<int32> MissingLayer;
    uint64 LayerRevision = UINT64_MAX;
    bool MixedLayer = false;
    bool Synchronizing = false;

    void ChangeLayer(const std::string& text)
    {
        auto* world = ResolveWorld();
        int32 layer = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), layer);
        if (world && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            ApplyRenderSortingLayer(*world, Entities, layer, Undo, Notifications);
    }

    void Refresh()
    {
        auto* world = ResolveWorld();
        if (!world || !world->IsValid(Entities.front()) || Layer->IsMenuOpen() ||
            Order->IsInFocusChain() || (Undo && Undo->HasOpenInteractiveEdit()))
            return;
        const auto* sorting = world->GetComponent<Sorting>(Entities.front());
        const int32 layer = sorting ? sorting->Layer : 0;
        const int32 order = sorting ? sorting->OrderInLayer : 0;
        bool mixedLayer = false;
        bool mixedOrder = false;
        for (const auto entity : Entities)
        {
            if (!world->IsValid(entity))
                continue;
            const auto* other = world->GetComponent<Sorting>(entity);
            mixedLayer |= (other ? other->Layer : 0) != layer;
            mixedOrder |= (other ? other->OrderInLayer : 0) != order;
        }
        const uint64 revision = Rendering::SortingLayers::Revision();
        const bool layersChanged = LayerRevision != revision;
        if (layersChanged)
        {
            auto layers = Rendering::SortingLayers::Current();
            std::sort(layers.Layers.begin(), layers.Layers.end(),
                [](const auto& left, const auto& right) { return left.Order < right.Order; });
            Options.clear();
            for (const auto& value : layers.Layers)
                Options.push_back({std::to_string(value.Id), value.Name});
            LayerRevision = revision;
        }
        const std::string value = std::to_string(layer);
        const auto found = std::find_if(Options.begin(), Options.end(),
            [&value](const auto& option) { return option.value == value; });
        const std::optional<int32> missing = found == Options.end() ? std::optional<int32>(layer) : std::nullopt;
        int selected = static_cast<int>(found - Options.begin());
        if (layersChanged || missing != MissingLayer || mixedLayer != MixedLayer)
        {
            auto options = Options;
            if (missing)
                options.push_back({value, "Missing layer (" + value + ")"});
            if (mixedLayer)
            {
                selected = static_cast<int>(options.size());
                options.push_back({"", "\xE2\x80\x94"});
            }
            Synchronizing = true;
            Layer->SetOptions(options, selected);
            Synchronizing = false;
            MissingLayer = missing;
            MixedLayer = mixedLayer;
        }
        else if (!mixedLayer && Layer->GetSelectedIndex() != selected)
            Layer->SetSelectedIndexWithoutNotify(selected);
        if (Order->GetValue() != order || Order->IsMixed() != mixedOrder)
        {
            Order->SetValueWithoutNotify(order);
            if (mixedOrder)
                Order->SetMixed();
        }
    }
};
}

void AddRenderSortingFields(const InspectorContext& context)
{
    if (!context.Parent || !context.World || !context.World->IsValid(context.Entity))
        return;
    auto state = std::make_shared<SortingFieldsState>();
    state->ResolveWorld = [expected = context.World, getWorld = context.GetWorld]() -> ECS::World*
    {
        return !getWorld || getWorld() == expected ? expected : nullptr;
    };
    state->Entities.push_back(context.Entity);
    for (const auto entity : InspectorDrag::GetAdditionalEntities(context))
        state->Entities.push_back(entity);
    state->Undo = context.Undo;
    state->Notifications = context.ChangeNotifications;
    state->Layer = InspectorUI::AddDropdownRow(context.Parent, "Sorting Layer", {}, 0,
        "Draw order for alpha-blended artwork. Layers are configured in Project Settings > Sorting Layers. Opaque surfaces use depth.");
    state->Layer->SetAutoWidthPopup(true);
    auto handlers = MakeRenderSortingOrderHandlers(context);
    state->Order = InspectorDrag::AddIntRowWithDrag(context.Parent, "Order in Layer", 0,
        std::move(handlers.first), std::move(handlers.second), 0,
        "Lower values draw first. Inside a parent sorting group, this controls member order.");
    state->Layer->SetOnValueChanged([state](const std::string& text)
    {
        if (!state->Synchronizing)
            state->ChangeLayer(text);
    });
    state->Refresh();
    if (context.SimulationRefreshCallbacks)
        context.SimulationRefreshCallbacks->push_back([state]() { state->Refresh(); });
    AddRenderSortingDiagnostic(context);
}
}
