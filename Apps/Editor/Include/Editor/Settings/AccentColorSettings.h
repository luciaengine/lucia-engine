#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

#include <cstdint>

namespace GameEngine
{
class UIManager;

namespace Editor
{

// The editor accent colour (Settings > UI Colors). Persisted in editor
// preferences and pushed as `:root` accent-variable overrides into every
// editor window that has been applied, so a change reaches all of them.
class AccentColorSettings
{
public:
    static AccentColorSettings& Get();

    uint32_t GetAccentColor() const { return m_AccentColor; }

    // persist=false updates the live windows only (colour-picker drag / cancel).
    void SetAccentColor(uint32_t argb, bool persist = true);

    // Push the current accent into `ui` and track it for later changes. Call
    // once per editor window after its theme is attached: the accent sheet
    // must follow the theme in sheet order to win the `:root` tie.
    void ApplyTo(UIManager* ui);

private:
    AccentColorSettings();
    void ApplyToWindow(UIManager& ui);

    uint32_t m_AccentColor;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
