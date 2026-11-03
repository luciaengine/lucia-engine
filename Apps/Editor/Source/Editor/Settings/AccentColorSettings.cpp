#include "Editor/Settings/AccentColorSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "UI/ThemeAccent.h"
#include "UI/UIAccentStyleHelper.h"

#include <string>

namespace GameEngine::Editor
{

AccentColorSettings& AccentColorSettings::Get()
{
    static AccentColorSettings instance;
    return instance;
}

AccentColorSettings::AccentColorSettings()
    : m_AccentColor(UI::kDefaultAccentArgb)
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    int64_t stored = static_cast<int64_t>(m_AccentColor);
    if (prefs.TryGetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, stored))
        m_AccentColor = static_cast<uint32_t>(stored);
}

void AccentColorSettings::SetAccentColor(uint32_t argb, bool persist)
{
    const bool colorChanged = m_AccentColor != argb;
    m_AccentColor = argb;
    if (persist)
    {
        SettingsStore prefs = OpenEditorPreferences();
        std::string error;
        prefs.Load(&error);
        prefs.SetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, static_cast<int64_t>(m_AccentColor));
        prefs.Save(&error);
    }
    if (colorChanged)
        m_Windows.ForEachLive([this](UIManager& ui) { ApplyToWindow(ui); });
}

void AccentColorSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyToWindow(*ui);
}

void AccentColorSettings::ApplyToWindow(UIManager& ui)
{
    m_Windows.SetOwnedStylesheet(ui, UI::AccentStyleHelper::BuildAccentColorStylesheet(m_AccentColor));
}

} // namespace GameEngine::Editor
