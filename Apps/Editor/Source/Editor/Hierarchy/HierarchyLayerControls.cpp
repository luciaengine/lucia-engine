#include "Editor/Hierarchy/HierarchyLayerControls.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderLayer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Platform/ContextMenu.h"
#include "Panels/HierarchyCommandIds.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "UI/Controls/Dropdown.h"
#include "UI/EditorIcons.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <optional>

namespace GameEngine::Editor
{
namespace
{
// Kept on the pooled row: changing the entity must not rebuild its popup.
class HierarchyLayerPicker final : public Dropdown
{
public:
    void PresentSorting(int32 current)
    {
        const auto revision = Rendering::SortingLayers::Revision();
        if (!m_Revision || *m_Revision != revision)
        {
            auto layers = Rendering::SortingLayers::Current();
            std::sort(layers.Layers.begin(), layers.Layers.end(),
                [](const auto& a, const auto& b) { return a.Order < b.Order; });
            m_Base.clear();
            for (const auto& layer : layers.Layers)
                m_Base.push_back({std::to_string(layer.Id), layer.Name});
            m_Revision = revision;
            m_Installed = false;
        }
        const auto value = std::to_string(current);
        const auto found = std::find_if(m_Base.begin(), m_Base.end(),
            [&](const Option& option) { return option.value == value; });
        const std::optional<int32> missing = found == m_Base.end()
            ? std::optional<int32>(current) : std::nullopt;
        const int selected = static_cast<int>(found - m_Base.begin());
        if (!m_Installed || missing != m_Missing)
        {
            auto options = m_Base;
            if (missing)
                options.push_back({value, "Missing layer (" + value + ")"});
            SetOptions(options, selected);
            m_Missing = missing;
            m_Installed = true;
        }
        else if (GetSelectedIndex() != selected)
            SetSelectedIndexWithoutNotify(selected);
    }

    void PresentVisibility(uint32 mask)
    {
        const std::optional<uint32> extra = mask == 0 || (mask & (mask - 1u)) != 0
            ? std::optional<uint32>(mask) : std::nullopt;
        int selected = 0;
        if (!m_Installed || extra != m_ExtraMask)
        {
            std::vector<Option> options;
            if (extra)
            {
                char label[32]{};
                std::snprintf(label, sizeof(label), "Mask 0x%08X", mask);
                options.push_back({std::to_string(mask), label});
            }
            for (uint32 layer = 0; layer < 32; ++layer)
                options.push_back({std::to_string(uint32{1} << layer), "Layer " + std::to_string(layer)});
            SetOptions(options, 0);
            m_ExtraMask = extra;
            m_Installed = true;
        }
        if (!extra)
            for (uint32 layer = 0; layer < 32; ++layer)
                if ((uint32{1} << layer) == mask)
                    selected = static_cast<int>(layer);
        if (GetSelectedIndex() != selected)
            SetSelectedIndexWithoutNotify(selected);
    }
private:
    std::optional<uint64> m_Revision;
    std::optional<int32> m_Missing;
    std::optional<uint32> m_ExtraMask;
    std::vector<Option> m_Base;
    bool m_Installed = false;
};

HierarchyLayerPicker& Picker(UIElement& row, const char* className)
{
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass(className))
            return *static_cast<HierarchyLayerPicker*>(child.get());
    auto picker = std::make_unique<HierarchyLayerPicker>();
    picker->AddClass("hierarchy-layer-picker");
    picker->AddClass(className);
    picker->SetAutoWidthPopup(true);
    picker->FitPopupToAvailableHeight();
    for (const auto& child : picker->GetChildren())
        if (child->HasClass("dropdown-header"))
            child->AddClass("hierarchy-layer-picker-header");
    auto* result = picker.get();
    row.AddChild(std::move(picker));
    return *result;
}

void SetClass(UIElement& element, const char* name, bool enabled)
{
    if (enabled)
        element.AddClass(name);
    else
        element.RemoveClass(name);
}
}

void HierarchyLayerControls::Present(ECS::World& world, ECS::EntityHandle entity,
    UIElement& row, UIElement& title, float panelWidth,
    const std::function<std::vector<ECS::EntityHandle>()>& targets,
    UndoRedoService* undo, EditorChangeNotifications* notifications)
{
    auto& visibility = Picker(row, "hierarchy-visibility-picker");
    auto& sorting = Picker(row, "hierarchy-sorting-picker");
    const uint32* visibilityMask = nullptr;
    if (const auto* layer = world.GetComponent<Components::RenderLayer>(entity))
        visibilityMask = &layer->mask;
    else if (const auto* mesh = world.GetComponent<Components::MeshRenderer>(entity))
        visibilityMask = &mesh->renderLayerMask;
    else if (const auto* skinned = world.GetComponent<Components::SkinnedMeshRenderer>(entity))
        visibilityMask = &skinned->renderLayerMask;
    const bool showVisibility = m_ShowVisibility && visibilityMask;
    const bool showSorting = m_ShowSorting && SupportsRenderSorting(world, entity);
    UI::Layout::SetElementHidden(visibility, !showVisibility);
    UI::Layout::SetElementHidden(sorting, !showSorting);
    const bool narrow = panelWidth < 400.0f;
    SetClass(visibility, "hierarchy-layer-picker-narrow", narrow);
    SetClass(sorting, "hierarchy-layer-picker-narrow", narrow);
    SetClass(sorting, "hierarchy-layer-picker-second", showVisibility && !narrow);
    SetClass(sorting, "hierarchy-layer-picker-second-narrow", showVisibility && narrow);
    SetClass(title, "hierarchy-title-one-layer", (showVisibility != showSorting) && !narrow);
    SetClass(title, "hierarchy-title-two-layers", showVisibility && showSorting && !narrow);
    SetClass(title, "hierarchy-title-one-layer-narrow", (showVisibility != showSorting) && narrow);
    SetClass(title, "hierarchy-title-two-layers-narrow", showVisibility && showSorting && narrow);
    SetClass(row, "hierarchy-row-with-layers", showVisibility || showSorting);
    if (showVisibility)
    {
        visibility.SetOnValueChanged({});
        visibility.PresentVisibility(*visibilityMask);
        visibility.SetTooltip("Render Layer: visibility filtering");
        visibility.SetOnValueChanged([&world, targets, undo, notifications](const std::string& text)
        {
            uint32 value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            {
                ApplyRenderLayerMask(world, targets(), value, undo, notifications);
            }
        });
    }
    if (showSorting)
    {
        const auto* component = world.GetComponent<Components::RenderSorting>(entity);
        const int32 current = component ? component->Layer : 0;
        sorting.SetOnValueChanged({});
        sorting.PresentSorting(current);
        sorting.SetTooltip("Sorting Layer: draw order for alpha-blended artwork");
        sorting.SetOnValueChanged([&world, targets, undo, notifications](const std::string& text)
        {
            int32 layer = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), layer);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
                ApplyRenderSortingLayer(world, targets(), layer, undo, notifications);
        });
    }
}

void HierarchyLayerControls::BuildMenu(ContextMenuBuilder& builder) const
{
    builder.AddItem("Hierarchy Options/Show Render Layer", HierarchyCommandIds::kCmdShowRenderLayer,
        m_ShowVisibility ? MenuItemFlag_Checked : MenuItemFlag_None, 70, EditorIcons::kEye);
    builder.AddItem("Hierarchy Options/Show Sorting Layer", HierarchyCommandIds::kCmdShowSortingLayer,
        m_ShowSorting ? MenuItemFlag_Checked : MenuItemFlag_None, 71, EditorIcons::kEye);
}

void HierarchyLayerControls::Hide(UIElement& row)
{
    row.RemoveClass("hierarchy-row-with-layers");
    for (const auto& child : row.GetChildren())
    {
        if (child->HasClass("hierarchy-layer-picker"))
            UI::Layout::SetElementHidden(*child, true);
        child->RemoveClass("hierarchy-title-one-layer");
        child->RemoveClass("hierarchy-title-two-layers");
        child->RemoveClass("hierarchy-title-one-layer-narrow");
        child->RemoveClass("hierarchy-title-two-layers-narrow");
    }
}

bool HierarchyLayerControls::NeedsRefresh(ECS::World& world)
{
    const auto revision = Rendering::SortingLayers::Revision();
    const auto reset = world.GetLifecycleResetGeneration();
    bool changed = m_LayersRevision != revision || m_World != &world || m_ResetGeneration != reset;
    if (m_World != &world || m_ResetGeneration != reset)
        m_Changes.LastRunVersion = 0;
    m_World = &world;
    m_ResetGeneration = reset;
    m_LayersRevision = revision;
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    if (m_ShowSorting && !changed)
    {
        auto query = world.Query<ECS::Read<Components::RenderSorting>>();
        query.IncludeDisabled();
        query.Changed<Components::RenderSorting>(m_Changes);
        query.BatchEach([&changed](const Components::RenderSorting*, std::size_t) { changed = true; });
    }
    m_Changes.LastRunVersion = entryVersion;
    return changed;
}

bool HierarchyLayerControls::HandleCommand(uint32_t command)
{
    if (command == HierarchyCommandIds::kCmdShowRenderLayer)
        m_ShowVisibility = !m_ShowVisibility;
    else if (command == HierarchyCommandIds::kCmdShowSortingLayer)
        m_ShowSorting = !m_ShowSorting;
    else
        return false;
    return true;
}
}
