#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Inspector header height (Settings > Inspector > Compact Headers): 24px when
// on, 28px when off, for component headers and the section headers inside
// them. Persisted in editor preferences and pushed as a stylesheet into every
// editor window that has been applied, so a change reaches all of them.
class CompactComponentHeadersSettings
{
public:
    static constexpr bool kDefaultEnabled = true;

    static CompactComponentHeadersSettings& Get();

    bool GetEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled);

    // Push the current header height into `ui` and track it for later changes.
    // Call once per editor window after its theme is attached.
    void ApplyTo(UIManager* ui);

private:
    CompactComponentHeadersSettings();
    void ApplyToWindow(UIManager& ui);

    bool m_Enabled = kDefaultEnabled;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
