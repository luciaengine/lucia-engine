#include "Editor/Hierarchy/HierarchyLayerControls.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Inspectors/RenderLayers/RenderLayerField.h"
#include "Panels/HierarchyCommandIds.h"
#include "Platform/ContextMenu.h"
#include "Rendering/RenderLayerMask.h"
#include "Rendering/RenderLayersScriptApi.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "UI/Controls/Dropdown.h"
#include "UI/EditorIcons.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kRenderLayerClass = "hierarchy-render-layer-dropdown";
constexpr const char* kSortingLayerClass = "hierarchy-sorting-layer-dropdown";
// Below this panel width both pickers take their short form, so the name keeps usable room.
constexpr float kNarrowPanelPx = 400.0f;

// Kept on the pooled row, so a row that shows another entity keeps its popup and only moves its pick.
class SortingLayerPicker final : public Dropdown
{
  public:
    void Present(int32 current)
    {
        const uint64 revision = Rendering::SortingLayers::Revision();
        const bool layersChanged = !m_Revision || *m_Revision != revision;
        if (layersChanged)
        {
            m_Options = SortingLayerOptions();
            m_Revision = revision;
        }
        const std::string value = std::to_string(current);
        const auto found = std::find_if(m_Options.begin(), m_Options.end(),
                                        [&value](const Option& option) { return option.value == value; });
        const std::optional<int32> missing = found == m_Options.end() ? std::optional<int32>(current) : std::nullopt;
        const int selected = static_cast<int>(found - m_Options.begin());
        if (layersChanged || missing != m_Missing)
        {
            std::vector<Option> options = m_Options;
            if (missing)
                options.push_back(MissingSortingLayerOption(*missing));
            SetOptions(options, selected);
            m_Missing = missing;
        }
        else if (GetSelectedIndex() != selected)
            SetSelectedIndexWithoutNotify(selected);
    }

  private:
    std::optional<uint64> m_Revision;
    std::optional<int32> m_Missing;
    std::vector<Option> m_Options;
};

// One project-named layer per option. A mask with more than one layer gets a "Mask 0x..." option first, so
// the row still shows the stored value; a pick sets exactly one layer.
class RenderLayerPicker final : public Dropdown
{
  public:
    void Present(uint32 mask, uint64 namesRevision)
    {
        const bool oneLayer = mask != 0 && (mask & (mask - 1u)) == 0;
        const std::optional<uint32> multiLayer = oneLayer ? std::nullopt : std::optional<uint32>(mask);
        if (!m_NamesRevision || *m_NamesRevision != namesRevision || multiLayer != m_MultiLayer)
        {
            SetOptions(RenderLayerOptions(multiLayer), 0);
            m_NamesRevision = namesRevision;
            m_MultiLayer = multiLayer;
        }
        const int selected = oneLayer ? static_cast<int>(std::countr_zero(mask)) : 0;
        if (GetSelectedIndex() != selected)
            SetSelectedIndexWithoutNotify(selected);
    }

  private:
    static std::vector<Option> RenderLayerOptions(std::optional<uint32> multiLayer)
    {
        std::vector<Option> options;
        if (multiLayer)
        {
            char label[32]{};
            std::snprintf(label, sizeof(label), "Mask 0x%08X", *multiLayer);
            options.push_back({std::to_string(*multiLayer), label});
        }
        for (uint32 layer = 0; layer < Rendering::RenderLayerMask::kCount; ++layer)
        {
            const Rendering::RenderLayerMask single = Rendering::RenderLayerMask::FromLayer(layer);
            options.push_back({std::to_string(single.Bits), Rendering::Script::NameOf(single)});
        }
        return options;
    }

    std::optional<uint64> m_NamesRevision;
    std::optional<uint32> m_MultiLayer;
};

template <typename Picker>
Picker* FindPicker(UIElement& row, const char* className)
{
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass(className))
            return dynamic_cast<Picker*>(child.get());
    return nullptr;
}

RenderLayerPicker& RenderPicker(UIElement& row)
{
    if (auto* picker = FindPicker<RenderLayerPicker>(row, kRenderLayerClass))
        return *picker;
    auto picker = std::make_unique<RenderLayerPicker>();
    picker->AddClass(kRenderLayerClass);
    picker->SetAutoWidthPopup(true);
    picker->FitPopupToAvailableHeight();
    picker->SetTooltip("Render Layer: the views that draw this entity");
    RenderLayerPicker& result = *picker;
    row.AddChild(std::move(picker));
    return result;
}

SortingLayerPicker& SortingPicker(UIElement& row)
{
    if (auto* picker = FindPicker<SortingLayerPicker>(row, kSortingLayerClass))
        return *picker;
    auto picker = std::make_unique<SortingLayerPicker>();
    picker->AddClass(kSortingLayerClass);
    picker->SetAutoWidthPopup(true);
    picker->FitPopupToAvailableHeight();
    picker->SetTooltip("Sorting Layer: the draw order of alpha-blended artwork");
    SortingLayerPicker& result = *picker;
    row.AddChild(std::move(picker));
    return result;
}

void SetClass(UIElement& element, const char* name, bool enabled)
{
    if (enabled)
        element.AddClass(name);
    else
        element.RemoveClass(name);
}

template <typename Value>
bool ParseValue(const std::string& text, Value& out)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
} // namespace

HierarchyLayerControls::HierarchyLayerControls()
    : m_NamesRefreshed(RenderLayerFieldsRefreshed().Subscribe([this]() { ++m_NamesRevision; }))
{
}

void HierarchyLayerControls::Present(ECS::World& world, ECS::EntityHandle entity, UIElement& row,
                                     float panelWidth, TargetsResolver targets, UndoRedoService* undo,
                                     EditorChangeNotifications* notifications)
{
    const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity);
    const bool showRenderLayer = m_ShowRenderLayer && renderer;
    const bool showSortingLayer = m_ShowSortingLayer && SupportsRenderSorting(world, entity);

    // Created on first need only, so a Hierarchy with both columns off adds nothing to its rows.
    RenderLayerPicker* renderPicker =
        showRenderLayer ? &RenderPicker(row) : FindPicker<RenderLayerPicker>(row, kRenderLayerClass);
    SortingLayerPicker* sortingPicker =
        showSortingLayer ? &SortingPicker(row) : FindPicker<SortingLayerPicker>(row, kSortingLayerClass);
    if (renderPicker)
        UI::Layout::SetElementHidden(*renderPicker, !showRenderLayer);
    if (sortingPicker)
        UI::Layout::SetElementHidden(*sortingPicker, !showSortingLayer);

    // The pickers are anchored to the row's right edge, not flowed, so the row's classes make the title
    // reserve their strip (theme/views.css).
    SetClass(row, "has-render-layer", showRenderLayer);
    SetClass(row, "has-sorting-layer", showSortingLayer);
    SetClass(row, "render-layer-narrow", (showRenderLayer || showSortingLayer) && panelWidth < kNarrowPanelPx);

    if (showRenderLayer)
    {
        renderPicker->SetOnValueChanged({});
        renderPicker->Present(renderer->RenderLayers.Bits, m_NamesRevision);
        renderPicker->SetOnValueChanged([&world, targets, undo, notifications](const std::string& text)
        {
            // A row inside the selection applies to the whole selection.
            uint32 bits = 0;
            if (ParseValue(text, bits))
                ApplyRenderLayerMask(world, targets(), Rendering::RenderLayerMask{bits}, undo, notifications);
        });
    }
    if (showSortingLayer)
    {
        const auto* sorting = world.GetComponent<Components::RenderSorting>(entity);
        sortingPicker->SetOnValueChanged({});
        sortingPicker->Present(sorting ? sorting->Layer : 0);
        sortingPicker->SetOnValueChanged([&world, targets, undo, notifications](const std::string& text)
        {
            int32 layer = 0;
            if (ParseValue(text, layer))
                ApplyRenderSortingLayer(world, targets(), layer, undo, notifications);
        });
    }
}

void HierarchyLayerControls::Hide(UIElement& row)
{
    row.RemoveClass("has-render-layer");
    row.RemoveClass("has-sorting-layer");
    row.RemoveClass("render-layer-narrow");
    for (const char* className : {kRenderLayerClass, kSortingLayerClass})
        if (UIElement* picker = FindPicker<UIElement>(row, className))
            UI::Layout::SetElementHidden(*picker, true);
}

void HierarchyLayerControls::BuildMenu(ContextMenuBuilder& builder) const
{
    builder.AddItem("Hierarchy Options/Show Render Layer", HierarchyCommandIds::kCmdShowRenderLayer,
                    m_ShowRenderLayer ? MenuItemFlag_Checked : MenuItemFlag_None, 70, EditorIcons::kEye);
    builder.AddItem("Hierarchy Options/Show Sorting Layer", HierarchyCommandIds::kCmdShowSortingLayer,
                    m_ShowSortingLayer ? MenuItemFlag_Checked : MenuItemFlag_None, 71, EditorIcons::kEye);
}

bool HierarchyLayerControls::HandleCommand(uint32_t command)
{
    if (command == HierarchyCommandIds::kCmdShowRenderLayer)
        m_ShowRenderLayer = !m_ShowRenderLayer;
    else if (command == HierarchyCommandIds::kCmdShowSortingLayer)
        m_ShowSortingLayer = !m_ShowSortingLayer;
    else
        return false;
    return true;
}

bool HierarchyLayerControls::NeedsRefresh(ECS::World& world)
{
    const uint64 revision = Rendering::SortingLayers::Revision();
    const uint64 reset = world.GetLifecycleResetGeneration();
    const bool otherWorld = m_World != &world || m_ResetGeneration != reset;
    bool changed = otherWorld || m_SortingLayersRevision != revision;
    if (otherWorld)
        m_SortingChanges.LastRunVersion = 0;
    m_World = &world;
    m_ResetGeneration = reset;
    m_SortingLayersRevision = revision;
    // ChangeGate contract: sample before the scan, write the sample back after it.
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    if (m_ShowSortingLayer && !changed)
    {
        auto query = world.Query<ECS::Read<Components::RenderSorting>>();
        query.IncludeDisabled();
        query.Changed<Components::RenderSorting>(m_SortingChanges);
        query.BatchEach([&changed](const Components::RenderSorting*, std::size_t) { changed = true; });
    }
    m_SortingChanges.LastRunVersion = entryVersion;
    const bool namesChanged = m_RefreshedNamesRevision != m_NamesRevision;
    m_RefreshedNamesRevision = m_NamesRevision;
    return (changed && m_ShowSortingLayer) || (namesChanged && m_ShowRenderLayer);
}

} // namespace GameEngine::Editor
