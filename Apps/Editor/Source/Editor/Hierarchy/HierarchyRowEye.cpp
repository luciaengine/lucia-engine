#include "Editor/Hierarchy/HierarchyRowEye.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kShowEyePreference = "hierarchy.showSceneViewEye";

std::once_flag s_ShownLoaded;
std::atomic<bool> s_Shown{true};
// Moves on every change, so each Hierarchy panel restyles its rows once.
std::atomic<uint64_t> s_ShownRevision{0};

void LoadShownPreference()
{
    auto preferences = OpenEditorPreferences();
    (void)preferences.Load(nullptr);
    bool shown = true;
    (void)preferences.TryGetBool(kShowEyePreference, shown);
    s_Shown.store(shown, std::memory_order_relaxed);
}
} // namespace

bool IsHierarchyEyeShown()
{
    std::call_once(s_ShownLoaded, LoadShownPreference);
    return s_Shown.load(std::memory_order_relaxed);
}

void SetHierarchyEyeShown(bool shown)
{
    if (shown == IsHierarchyEyeShown())
        return;
    s_Shown.store(shown, std::memory_order_relaxed);
    s_ShownRevision.fetch_add(1, std::memory_order_relaxed);
    auto preferences = OpenEditorPreferences();
    (void)preferences.Load(nullptr);
    preferences.SetBool(kShowEyePreference, shown);
    (void)preferences.Save(nullptr);
}

void RegisterHierarchySettingsCategory()
{
    SettingsCategoryDescriptor hierarchy;
    hierarchy.CategoryId = "hierarchy";
    hierarchy.Title = "Hierarchy";
    hierarchy.TreeRowClass = "ui-hierarchy-row";
    hierarchy.Group = SettingsCategoryGroup::UI;
    hierarchy.SearchKeywords = "hierarchy rows eye visibility on off enable entity icon";
    hierarchy.Description = "How Hierarchy rows look.";

    SettingsFieldDescriptor showEye;
    showEye.Label = "Show Eye";
    showEye.Tooltip =
        "Show an eye at the end of each Hierarchy row. The eye and the entity icon show the same "
        "on/off state, and a click on either one turns the entity on or off.";
    showEye.SearchKeywords = "hierarchy eye visibility on off enable entity";
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = true;
    // No PrefKey: the eye owns the stored key and the cached value the rows read.
    toggle.Get = [] { return IsHierarchyEyeShown(); };
    toggle.Set = [](bool shown) { SetHierarchyEyeShown(shown); };
    showEye.Control = std::move(toggle);
    hierarchy.Fields.push_back(std::move(showEye));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(hierarchy));
}

HierarchyRowEye::HierarchyRowEye() : m_ShownRevision(s_ShownRevision.load(std::memory_order_relaxed))
{
}

void HierarchyRowEye::AddTo(UIElement& row)
{
    auto eye = std::make_unique<UIElement>();
    eye->AddClass("hierarchy-eye");
    eye->AddClass("tree-icon-hit-target");
    eye->SetDisableClipCulling(true);
    row.AddChild(std::move(eye));
}

void HierarchyRowEye::Present(UIElement& row, ECS::EntityHandle entity)
{
    const bool shown = IsHierarchyEyeShown() && entity.IsValid();
    for (const auto& child : row.GetChildren())
    {
        if (child && child->HasClass("hierarchy-eye"))
            UI::Layout::SetElementHidden(*child, !shown);
    }
}

bool HierarchyRowEye::ConsumeShownChange()
{
    const uint64_t revision = s_ShownRevision.load(std::memory_order_relaxed);
    if (revision == m_ShownRevision)
        return false;
    m_ShownRevision = revision;
    return true;
}

} // namespace GameEngine::Editor
