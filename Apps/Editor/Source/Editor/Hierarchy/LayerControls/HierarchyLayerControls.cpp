#include "Editor/Hierarchy/LayerControls/HierarchyLayerControls.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"
#include "Editor/Hierarchy/LayerControls/HierarchyLayerPicker.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Inspectors/RenderLayers/RenderLayerField.h"
#include "Panels/HierarchyCommandIds.h"
#include "Platform/ContextMenu.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "UI/EditorIcons.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <charconv>
#include <memory>
#include <string>

namespace GameEngine::Editor
{
namespace
{
// Below this panel width both columns take their narrow width.
constexpr float kNarrowPanelPx = 400.0f;

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
    HierarchyLayerPicker* result = picker.get();
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

template <typename Integer>
bool ParseWhole(const std::string& text, Integer& out)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
} // namespace

HierarchyLayerControls::HierarchyLayerControls()
    : m_NamesRefreshed(RenderLayerFieldsRefreshed().Subscribe([this]() { ++m_NamesRevision; }))
{
}

void HierarchyLayerControls::Present(ECS::World& world, ECS::EntityHandle entity, UIElement& row, UIElement& title,
                                     float panelWidth, const std::function<std::vector<ECS::EntityHandle>()>& targets,
                                     UndoRedoService* undo, EditorChangeNotifications* notifications)
{
    HierarchyLayerPicker& visibility = Picker(row, "hierarchy-visibility-picker");
    HierarchyLayerPicker& sorting = Picker(row, "hierarchy-sorting-picker");
    const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity);
    const bool showVisibility = m_ShowVisibility && renderer;
    const bool showSorting = m_ShowSorting && SupportsRenderSorting(world, entity);
    UI::Layout::SetElementHidden(visibility, !showVisibility);
    UI::Layout::SetElementHidden(sorting, !showSorting);
    const bool narrow = panelWidth < kNarrowPanelPx;
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
        visibility.PresentVisibility(renderer->RenderLayers.Bits, m_NamesRevision);
        visibility.SetTooltip("Render Layer: the views that draw this entity");
        visibility.SetOnValueChanged([&world, targets, undo, notifications](const std::string& text)
        {
            uint32 bits = 0;
            if (ParseWhole(text, bits))
                ApplyRenderLayerMask(world, targets(), Rendering::RenderLayerMask{bits}, undo, notifications);
        });
    }
    if (showSorting)
    {
        const auto* component = world.GetComponent<Components::RenderSorting>(entity);
        sorting.SetOnValueChanged({});
        sorting.PresentSorting(component ? component->Layer : 0);
        sorting.SetTooltip("Sorting Layer: draw order for alpha-blended artwork");
        sorting.SetOnValueChanged([&world, targets, undo, notifications](const std::string& text)
        {
            int32 layer = 0;
            if (ParseWhole(text, layer))
                ApplyRenderSortingLayer(world, targets(), layer, undo, notifications);
        });
    }
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

void HierarchyLayerControls::BuildMenu(ContextMenuBuilder& builder) const
{
    builder.AddItem("Hierarchy Options/Show Render Layer", HierarchyCommandIds::kCmdShowRenderLayer,
                    m_ShowVisibility ? MenuItemFlag_Checked : MenuItemFlag_None, 70, EditorIcons::kEye);
    builder.AddItem("Hierarchy Options/Show Sorting Layer", HierarchyCommandIds::kCmdShowSortingLayer,
                    m_ShowSorting ? MenuItemFlag_Checked : MenuItemFlag_None, 71, EditorIcons::kEye);
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

bool HierarchyLayerControls::NeedsRefresh(ECS::World& world)
{
    const auto revision = Rendering::SortingLayers::Revision();
    const auto reset = world.GetLifecycleResetGeneration();
    bool changed = m_LayersRevision != revision || m_World != &world || m_ResetGeneration != reset ||
                   m_PresentedNamesRevision != m_NamesRevision;
    if (m_World != &world || m_ResetGeneration != reset)
        m_Changes.LastRunVersion = 0;
    m_World = &world;
    m_ResetGeneration = reset;
    m_LayersRevision = revision;
    m_PresentedNamesRevision = m_NamesRevision;
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

} // namespace GameEngine::Editor
