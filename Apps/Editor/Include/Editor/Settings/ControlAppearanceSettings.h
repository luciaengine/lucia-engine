#pragma once

#include <vector>

#include "UI/UIManagerRef.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// User preferences for how controls are drawn: the switch-track inset shadow and the dropdown
// outline. Persisted in editor preferences. Get() registers the Settings descriptor; ApplyTo stamps
// the root classes on a UIManager and tracks it for live Set.
class ControlAppearanceSettings
{
public:
    static ControlAppearanceSettings& Get();

    bool GetInnerShadowEnabled() const { return m_InnerShadowEnabled; }
    bool GetDropdownOutlinesEnabled() const { return m_DropdownOutlinesEnabled; }

    // Live apply only. Persistence is the Settings row's PrefKey — these
    // methods must not write the file, because the row also fires Set with
    // Get()'s own value when the page is built.
    void SetInnerShadowEnabled(bool enabled);
    void SetDropdownOutlinesEnabled(bool enabled);

    // Stamp the root classes on `ui` and remember it so Set can update
    // every window that has already been applied.
    void ApplyTo(UIManager* ui);

private:
    ControlAppearanceSettings();
    void ApplyToTrackedManagers();
    void ApplyClasses(UIManager* ui) const;

    bool m_InnerShadowEnabled = false;
    bool m_DropdownOutlinesEnabled = true;
    std::vector<UIManagerRef> m_TrackedManagers;
};

void RegisterControlAppearanceSettings();

} // namespace Editor
} // namespace GameEngine
