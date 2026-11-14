#include "Editor/Settings/ValueBoxHeightSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "Types/StringId.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <cstdio>
#include <string>

namespace GameEngine::Editor
{

ValueBoxHeightSettings& ValueBoxHeightSettings::Get()
{
    static ValueBoxHeightSettings instance;
    return instance;
}

ValueBoxHeightSettings::ValueBoxHeightSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    double stored = m_HeightPx;
    if (prefs.TryGetDouble(kPrefKey, stored))
        m_HeightPx = static_cast<float>(stored);
}

void ValueBoxHeightSettings::SetHeightPx(float heightPx)
{
    m_HeightPx = heightPx;
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyToWindow(ui); });
}

void ValueBoxHeightSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyToWindow(*ui);
}

void ValueBoxHeightSettings::ApplyToWindow(UIManager& ui) const
{
    UIElement* root = ui.GetRootElement();
    if (!root)
        return;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0fpx", m_HeightPx);
    root->Overrides().SetCustom(HashStringId("--value_box_height"), buf);
    ui.MarkStyleDirtyAll();
}

} // namespace GameEngine::Editor
