#include "Editor/Settings/TabIconsSettings.h"

#include "Editor/Settings/SettingsStore.h"

#include <string>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kPrefKey = "ui.tabIcons";
// Set when the icons are off.
constexpr const char* kRootClass = "no-tab-icons";

} // namespace

TabIconsSettings& TabIconsSettings::Get()
{
    static TabIconsSettings instance;
    return instance;
}

TabIconsSettings::TabIconsSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.TryGetBool(kPrefKey, m_Enabled);
}

void TabIconsSettings::SetEnabled(bool enabled)
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.SetBool(kPrefKey, enabled);
    prefs.Save(&error);

    if (m_Enabled == enabled)
        return;
    m_Enabled = enabled;
    m_Windows.ForEachLive([this](UIManager& ui)
                          { TrackedEditorWindows::SetRootClass(ui, kRootClass, !m_Enabled); });
}

void TabIconsSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    TrackedEditorWindows::SetRootClass(*ui, kRootClass, !m_Enabled);
}

} // namespace GameEngine::Editor
