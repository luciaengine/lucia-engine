#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Icons on dock panel tabs (Settings > UI Appearance > Panel Tab Icons). Persisted in editor
// preferences; turning them off stamps the `no-tab-icons` root class on every
// editor window that has been applied, so a change reaches all of them.
class TabIconsSettings
{
public:
    static constexpr bool kDefaultEnabled = true;

    static TabIconsSettings& Get();

    bool GetEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled);

    // Stamp the root class on `ui` and track it for later changes. Call once
    // per editor window after its root exists.
    void ApplyTo(UIManager* ui);

private:
    TabIconsSettings();

    bool m_Enabled = kDefaultEnabled;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
