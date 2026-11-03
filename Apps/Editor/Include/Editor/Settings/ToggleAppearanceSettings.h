#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

#include "UI/UIManagerRef.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// How boolean toggles are drawn: square checkmarks instead of sliding
// switches, and the switch-track inset shadow. Persisted in editor
// preferences. Get() registers the inner-shadow Settings descriptor; ApplyTo
// stamps a UIManager and tracks it for live Set.
class ToggleAppearanceSettings
{
public:
    static constexpr bool kDefaultCheckmarksEnabled = false;
    // The Settings row for checkmarks persists through this key.
    static constexpr const char* kPrefKeyCheckmarks = "ui.checkmarkToggles";

    static ToggleAppearanceSettings& Get();

    bool GetCheckmarksEnabled() const { return m_CheckmarksEnabled; }

    // Live apply only: the Settings row persists the value.
    void SetCheckmarksEnabled(bool enabled);

    bool GetInnerShadowEnabled() const { return m_InnerShadowEnabled; }

    // Live apply only. Persistence is the Settings row's PrefKey — this
    // method must not write the file, because the row also fires Set with
    // Get()'s own value when the page is built.
    void SetInnerShadowEnabled(bool enabled);

    // Stamp the toggle presentation on `ui` and remember it so Set can update
    // every window that has already been applied.
    void ApplyTo(UIManager* ui);

private:
    ToggleAppearanceSettings();
    void ApplyCheckmarks(UIManager& ui) const;

    bool m_CheckmarksEnabled = kDefaultCheckmarksEnabled;
    bool m_InnerShadowEnabled = false;
    TrackedEditorWindows m_Windows;
};

void RegisterToggleAppearanceSettings();

} // namespace Editor
} // namespace GameEngine
