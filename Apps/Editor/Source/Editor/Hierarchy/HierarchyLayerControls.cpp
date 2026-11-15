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
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/MaskField.h"
#include "UI/EditorIcons.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <charconv>
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

template <typename Picker>
Picker* FindPicker(UIElement& row, const char* className)
{
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass(className))
            return dynamic_cast<Picker*>(child.get());
    return nullptr;
}

MaskField& RenderLayerPicker(UIElement& row)
{
    if (auto* picker = FindPicker<MaskField>(row, kRenderLayerClass))
        return *picker;
    auto picker = std::make_unique<MaskField>();
    picker->AddClass(kRenderLayerClass);
    FeedRenderLayerNames(*picker);
    picker->SetNothingTooltip(kRenderLayersNothingTooltip);
    MaskField& result = *picker;
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

bool ParseLayer(const std::string& text, int32& out)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
} // namespace

void HierarchyLayerControls::Present(ECS::World& world, ECS::EntityHandle entity, UIElement& row,
                                     float panelWidth, TargetsResolver targets, UndoRedoService* undo,
                                     EditorChangeNotifications* notifications)
{
    const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity);
    const bool showRenderLayer = m_ShowRenderLayer && renderer;
    const bool showSortingLayer = m_ShowSortingLayer && SupportsRenderSorting(world, entity);

    // Created on first need only, so a Hierarchy with both columns off adds nothing to its rows.
    MaskField* renderPicker =
        showRenderLayer ? &RenderLayerPicker(row) : FindPicker<MaskField>(row, kRenderLayerClass);
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
        renderPicker->SetValueWithoutNotify(renderer->RenderLayers.Bits);
        renderPicker->SetOnBitsChanged([&world, targets, undo, notifications](uint32 bits, bool on)
        {
            // A row inside the selection applies to the whole selection; each entity keeps its other layers.
            ApplyRenderLayerBits(world, targets(), bits, on, undo, notifications);
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
            if (ParseLayer(text, layer))
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
    return changed && m_ShowSortingLayer;
}

} // namespace GameEngine::Editor
