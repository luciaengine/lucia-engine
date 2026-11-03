#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Gray sliders instead of accent-coloured ones (Settings > Inspector > Gray
// Sliders). Persisted in editor preferences; stamped as the `gray-sliders`
// root class on every editor window that has been applied, so a change
// reaches all of them.
class GraySlidersSettings
{
public:
    static constexpr bool kDefaultEnabled = false;

    static GraySlidersSettings& Get();

    bool GetEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled);

    // Stamp the root class on `ui` and track it for later changes. Call once
    // per editor window after its root exists.
    void ApplyTo(UIManager* ui);

private:
    GraySlidersSettings();

    bool m_Enabled = kDefaultEnabled;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
