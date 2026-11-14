#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Inspector row indent, row gap and label width (Settings > Inspector), plus
// the Settings page row spacing derived from the same gap. Pushed as one
// generated stylesheet into every editor window that has been applied, so a
// change reaches all of them.
class InspectorRowLayoutSettings
{
public:
    static constexpr float kDefaultIndentPx = 0.0f;
    static constexpr float kDefaultGapPx = 4.0f;
    static constexpr float kDefaultLabelWidthPercent = 37.0f;

    // The Settings sliders persist through these keys.
    static constexpr const char* kPrefKeyIndent = "ui.inspectorRowIndent";
    static constexpr const char* kPrefKeyGap = "ui.inspectorRowGap";
    static constexpr const char* kPrefKeyLabelWidth = "ui.inspectorLabelWidth";

    static InspectorRowLayoutSettings& Get();

    float GetIndentPx() const { return m_IndentPx; }
    float GetGapPx() const { return m_GapPx; }
    float GetLabelWidthPercent() const { return m_LabelWidthPercent; }

    // Live apply only: the Settings sliders persist their own value, and a
    // drag re-applies on every step. Replaces stylesheets, so it must not run
    // inside event dispatch; the Settings rows defer it.
    void SetLayout(float indentPx, float gapPx, float labelWidthPercent);

    // Push the current layout into `ui` and track it for later changes. Call
    // once per editor window after its theme is attached.
    void ApplyTo(UIManager* ui);

private:
    InspectorRowLayoutSettings();
    void ApplyToWindow(UIManager& ui);

    float m_IndentPx = kDefaultIndentPx;
    float m_GapPx = kDefaultGapPx;
    float m_LabelWidthPercent = kDefaultLabelWidthPercent;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
