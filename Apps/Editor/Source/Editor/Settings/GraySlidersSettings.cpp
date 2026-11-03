#include "Editor/Settings/GraySlidersSettings.h"

#include "Editor/Settings/SettingsStore.h"

#include <string>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kPrefKey = "ui.graySliders";
constexpr const char* kRootClass = "gray-sliders";

} // namespace

GraySlidersSettings& GraySlidersSettings::Get()
{
    static GraySlidersSettings instance;
    return instance;
}

GraySlidersSettings::GraySlidersSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.TryGetBool(kPrefKey, m_Enabled);
}

void GraySlidersSettings::SetEnabled(bool enabled)
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
                          { TrackedEditorWindows::SetRootClass(ui, kRootClass, m_Enabled); });
}

void GraySlidersSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    TrackedEditorWindows::SetRootClass(*ui, kRootClass, m_Enabled);
}

} // namespace GameEngine::Editor
