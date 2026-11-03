#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Editor text rendering (Settings > Font Rendering): subpixel AA opt-in, text
// contrast and text smoothing gamma. Applied to every editor window's
// UIManager that has been applied, so a change reaches all of them.
class TextRenderingSettings
{
public:
    static constexpr bool kDefaultSubpixelAA = false;
    static constexpr float kDefaultContrast = 1.0f;
    static constexpr float kMinContrast = 0.0f;
    static constexpr float kMaxContrast = 1.0f;
    static constexpr float kDefaultSmoothingGamma = 1.0f;
    static constexpr float kMinSmoothingGamma = 0.5f;
    static constexpr float kMaxSmoothingGamma = 2.0f;

    // The Settings rows persist through these keys.
    static constexpr const char* kPrefKeySubpixelAA = "text.subpixelAA";
    static constexpr const char* kPrefKeyContrast = "text.contrast";
    static constexpr const char* kPrefKeySmoothingGamma = "text.smoothingGamma";

    static TextRenderingSettings& Get();

    bool GetSubpixelAA() const { return m_SubpixelAA; }
    float GetContrast() const { return m_Contrast; }
    float GetSmoothingGamma() const { return m_SmoothingGamma; }

    // Live apply only: the Settings rows persist their own value, and a slider
    // drag re-applies on every step.
    void SetSubpixelAA(bool enabled);
    void SetContrast(float contrast);
    void SetSmoothingGamma(float gamma);

    // Apply the current text rendering to `ui` and track it for later changes.
    // Call once per editor window after it is created.
    void ApplyTo(UIManager* ui);

private:
    TextRenderingSettings();
    void ApplyToWindow(UIManager& ui) const;
    void ApplyToTrackedWindows();

    bool m_SubpixelAA = kDefaultSubpixelAA;
    float m_Contrast = kDefaultContrast;
    float m_SmoothingGamma = kDefaultSmoothingGamma;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
