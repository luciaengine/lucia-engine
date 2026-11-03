#include "Editor/Settings/PopupShadowSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "Types/StringId.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace GameEngine::Editor
{

PopupShadowSettings& PopupShadowSettings::Get()
{
    static PopupShadowSettings instance;
    return instance;
}

PopupShadowSettings::PopupShadowSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);

    prefs.TryGetBool(kPrefKeyDropdown, m_Shadow.Dropdown);
    prefs.TryGetBool(kPrefKeyPicker, m_Shadow.Picker);
    prefs.TryGetBool(kPrefKeyCompletion, m_Shadow.Completion);
    prefs.TryGetBool(kPrefKeyTooltip, m_Shadow.Tooltip);

    auto loadFloat = [&prefs](const char* key, float& out) {
        double stored = out;
        if (prefs.TryGetDouble(key, stored))
            out = static_cast<float>(stored);
    };
    loadFloat(kPrefKeyOffsetX, m_Shadow.OffsetX);
    loadFloat(kPrefKeyOffsetY, m_Shadow.OffsetY);
    loadFloat(kPrefKeyBlur, m_Shadow.Blur);
    loadFloat(kPrefKeyOpacity, m_Shadow.Opacity);
}

void PopupShadowSettings::SetShadow(const Shadow& shadow)
{
    m_Shadow = shadow;
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyToWindow(ui); });
}

void PopupShadowSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyToWindow(*ui);
}

// A `--popup-shadow` custom property on the root (consumed by
// `box-shadow: var(--popup-shadow)` in widgets.css) plus the per-popup opt-out
// root classes.
void PopupShadowSettings::ApplyToWindow(UIManager& ui) const
{
    UIElement* root = ui.GetRootElement();
    if (!root)
        return;

    const float opacity = std::clamp(m_Shadow.Opacity, 0.0f, 1.0f);
    char shadow[96];
    std::snprintf(shadow, sizeof(shadow), "%.1fpx %.1fpx %.1fpx rgba(0, 0, 0, %.3f)",
                  m_Shadow.OffsetX, m_Shadow.OffsetY, std::max(0.0f, m_Shadow.Blur), opacity);
    root->Overrides().SetCustom(HashStringId("--popup-shadow"), shadow);

    TrackedEditorWindows::SetRootClass(ui, "no-dropdown-shadow", !m_Shadow.Dropdown);
    TrackedEditorWindows::SetRootClass(ui, "no-picker-shadow", !m_Shadow.Picker);
    TrackedEditorWindows::SetRootClass(ui, "no-completion-shadow", !m_Shadow.Completion);
    TrackedEditorWindows::SetRootClass(ui, "no-tooltip-shadow", !m_Shadow.Tooltip);
}

} // namespace GameEngine::Editor
