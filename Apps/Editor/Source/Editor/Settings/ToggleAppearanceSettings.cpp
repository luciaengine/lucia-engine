#include "Editor/Settings/ToggleAppearanceSettings.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <unordered_set>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kInnerShadowPrefKey = "ui.toggleInnerShadow";
constexpr const char* kInnerShadowRootClass = "toggle-inner-shadow";

void SetCheckmarkClass(UIElement* element, bool enabled, std::unordered_set<UIElement*>& visited)
{
    if (!element || !visited.insert(element).second)
        return;

    if (auto* toggle = dynamic_cast<Toggle*>(element))
    {
        if (enabled)
            toggle->AddClass("toggle-checkmark");
        else
            toggle->RemoveClass("toggle-checkmark");
    }

    for (const auto& child : element->GetChildren())
        SetCheckmarkClass(child.get(), enabled, visited);
    SetCheckmarkClass(element->GetMountTarget(), enabled, visited);
}

} // namespace

ToggleAppearanceSettings& ToggleAppearanceSettings::Get()
{
    static ToggleAppearanceSettings instance;
    // After the singleton exists: RegisterCategory may replay into a
    // SettingsPanel observer, and those closures call Get().
    static const bool registered = []
    {
        RegisterToggleAppearanceSettings();
        return true;
    }();
    (void)registered;
    return instance;
}

ToggleAppearanceSettings::ToggleAppearanceSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.TryGetBool(kInnerShadowPrefKey, m_InnerShadowEnabled);
    prefs.TryGetBool(kPrefKeyCheckmarks, m_CheckmarksEnabled);
}

void ToggleAppearanceSettings::SetCheckmarksEnabled(bool enabled)
{
    m_CheckmarksEnabled = enabled;
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyCheckmarks(ui); });
}

void ToggleAppearanceSettings::SetInnerShadowEnabled(bool enabled)
{
    m_InnerShadowEnabled = enabled;
    m_Windows.ForEachLive([this](UIManager& ui)
                          { TrackedEditorWindows::SetRootClass(ui, kInnerShadowRootClass, m_InnerShadowEnabled); });
}

void ToggleAppearanceSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyCheckmarks(*ui);
    TrackedEditorWindows::SetRootClass(*ui, kInnerShadowRootClass, m_InnerShadowEnabled);
}

// Stamps toggle-checkmark on every Toggle in the window, including docked
// panels reached through their mount targets; new toggles read the
// process-wide presentation flag.
void ToggleAppearanceSettings::ApplyCheckmarks(UIManager& ui) const
{
    Toggle::SetCheckmarkPresentationEnabled(m_CheckmarksEnabled);

    std::unordered_set<UIElement*> visited;
    SetCheckmarkClass(ui.GetRootElement(), m_CheckmarksEnabled, visited);

    ui.MarkStyleDirtyAll();
    ui.RequestRelayout();
}

void RegisterToggleAppearanceSettings()
{
    SettingsCategoryDescriptor toggles;
    toggles.CategoryId = "toggles";
    toggles.Title = "Toggles";
    toggles.Group = SettingsCategoryGroup::UIAppearance;
    toggles.SearchKeywords = "toggle switch inner shadow inset checkbox";
    toggles.Description = "How boolean switch tracks are drawn.";

    SettingsFieldDescriptor innerShadow;
    innerShadow.Label = "Toggle Inner Shadow";
    innerShadow.Tooltip =
        "Recess switch tracks with a small inset shadow along the top and left "
        "inner edge. Square checkmark toggles are unchanged.";
    innerShadow.SearchKeywords = "toggle inner shadow inset recess switch track";
    innerShadow.PrefKey = kInnerShadowPrefKey;
    SettingsFieldDescriptor::ToggleField innerShadowToggle;
    innerShadowToggle.DefaultValue = false;
    innerShadowToggle.Get = []
    { return ToggleAppearanceSettings::Get().GetInnerShadowEnabled(); };
    innerShadowToggle.Set = [](bool enabled)
    { ToggleAppearanceSettings::Get().SetInnerShadowEnabled(enabled); };
    innerShadow.Control = std::move(innerShadowToggle);
    toggles.Fields.push_back(std::move(innerShadow));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(toggles));
}

} // namespace GameEngine::Editor
