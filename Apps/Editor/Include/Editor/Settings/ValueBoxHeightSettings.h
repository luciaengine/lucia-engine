#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Height of value boxes in the inspector and every settings input
// (Settings > UI Appearance > Value Box Height). Pushed as `--value_box_height`
// onto the root of every editor window that has been applied, so a change
// reaches all of them.
class ValueBoxHeightSettings
{
public:
    static constexpr float kDefaultHeightPx = 26.0f;
    // The Settings slider persists through this key.
    static constexpr const char* kPrefKey = "ui.valueBoxHeight";

    static ValueBoxHeightSettings& Get();

    float GetHeightPx() const { return m_HeightPx; }

    // Live apply only: the Settings slider persists the value, and a drag
    // re-applies on every step.
    void SetHeightPx(float heightPx);

    // Push the current height into `ui` and track it for later changes. Call
    // once per editor window after its root exists.
    void ApplyTo(UIManager* ui);

private:
    ValueBoxHeightSettings();
    void ApplyToWindow(UIManager& ui) const;

    float m_HeightPx = kDefaultHeightPx;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
