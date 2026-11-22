#include "Editor/Settings/ControlAppearanceSettings.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kInnerShadowPrefKey = "ui.toggleInnerShadow";
constexpr const char* kInnerShadowRootClass = "toggle-inner-shadow";
constexpr const char* kDropdownOutlinesPrefKey = "ui.dropdownOutlines";
constexpr const char* kNoDropdownOutlineRootClass = "dropdowns-no-outline";

void SetRootClass(UIElement& root, const char* name, bool enabled)
{
    if (enabled)
        root.AddClass(name);
    else
        root.RemoveClass(name);
}

bool GetInnerShadow()
{
    return ControlAppearanceSettings::Get().GetInnerShadowEnabled();
}

void SetInnerShadow(bool enabled)
{
    ControlAppearanceSettings::Get().SetInnerShadowEnabled(enabled);
}

bool GetDropdownOutlines()
{
    return ControlAppearanceSettings::Get().GetDropdownOutlinesEnabled();
}

void SetDropdownOutlines(bool enabled)
{
    ControlAppearanceSettings::Get().SetDropdownOutlinesEnabled(enabled);
}

SettingsFieldDescriptor MakeToggleRow(const char* label, const char* tooltip, const char* keywords,
                                      const char* prefKey, bool defaultValue, bool (*get)(), void (*set)(bool))
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.Tooltip = tooltip;
    field.SearchKeywords = keywords;
    field.PrefKey = prefKey;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = defaultValue;
    toggle.Get = get;
    toggle.Set = set;
    field.Control = std::move(toggle);
    return field;
}

} // namespace

ControlAppearanceSettings& ControlAppearanceSettings::Get()
{
    static ControlAppearanceSettings instance;
    // After the singleton exists: RegisterCategory may replay into a
    // SettingsPanel observer, and those closures call Get().
    static const bool registered = []
    {
        RegisterControlAppearanceSettings();
        return true;
    }();
    (void)registered;
    return instance;
}

ControlAppearanceSettings::ControlAppearanceSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.TryGetBool(kInnerShadowPrefKey, m_InnerShadowEnabled);
    prefs.TryGetBool(kDropdownOutlinesPrefKey, m_DropdownOutlinesEnabled);
}

void ControlAppearanceSettings::SetInnerShadowEnabled(bool enabled)
{
    m_InnerShadowEnabled = enabled;
    ApplyToTrackedManagers();
}

void ControlAppearanceSettings::SetDropdownOutlinesEnabled(bool enabled)
{
    m_DropdownOutlinesEnabled = enabled;
    ApplyToTrackedManagers();
}

void ControlAppearanceSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;

    bool alreadyTracked = false;
    for (const UIManagerRef& tracked : m_TrackedManagers)
    {
        if (tracked.Get() == ui)
        {
            alreadyTracked = true;
            break;
        }
    }
    if (!alreadyTracked)
        m_TrackedManagers.push_back(UIManagerRef(ui));

    ApplyClasses(ui);
}

void ControlAppearanceSettings::ApplyToTrackedManagers()
{
    size_t write = 0;
    for (const UIManagerRef& tracked : m_TrackedManagers)
    {
        UIManager* ui = tracked.Get();
        if (!ui)
            continue;
        ApplyClasses(ui);
        m_TrackedManagers[write++] = tracked;
    }
    m_TrackedManagers.resize(write);
}

void ControlAppearanceSettings::ApplyClasses(UIManager* ui) const
{
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;
    SetRootClass(*root, kInnerShadowRootClass, m_InnerShadowEnabled);
    SetRootClass(*root, kNoDropdownOutlineRootClass, !m_DropdownOutlinesEnabled);
    ui->MarkStyleDirtyAll();
}

void RegisterControlAppearanceSettings()
{
    SettingsCategoryDescriptor controls;
    controls.CategoryId = "controls";
    controls.Title = "Controls";
    controls.Group = SettingsCategoryGroup::UIAppearance;
    controls.SearchKeywords = "toggle switch inner shadow inset checkbox dropdown outline border";
    controls.Description = "How switches and dropdowns are drawn.";
    controls.Fields.push_back(MakeToggleRow(
        "Toggle Inner Shadow",
        "Recess switch tracks with a small inset shadow along the top and left inner edge. Square "
        "checkmark toggles are unchanged.",
        "toggle inner shadow inset recess switch track", kInnerShadowPrefKey, false, &GetInnerShadow,
        &SetInnerShadow));
    controls.Fields.push_back(MakeToggleRow(
        "Dropdown Outlines",
        "Draws an outline around dropdowns and asset fields. Off shows only their fill; the outline "
        "still shows under the pointer.",
        "dropdown outline border asset field picker select", kDropdownOutlinesPrefKey,
        ControlAppearanceSettings::kDropdownOutlinesDefault,
        &GetDropdownOutlines, &SetDropdownOutlines));
    EditorSettingsRegistry::Get().RegisterCategory(std::move(controls));
}

} // namespace GameEngine::Editor
