#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Popup drop shadows (Settings > UI Appearance > Popup Shadows): the shared
// `--popup-shadow` geometry and a per-popup opt-out. Pushed onto the root of
// every editor window that has been applied, so a change reaches all of them.
class PopupShadowSettings
{
public:
    // Default geometry matches the editor's modal dialogs. Opacity is tuned for
    // encoded-space (sRGB) UI blending, where black lands heavier than the same
    // alpha in linear light; keep the widgets.css var(--popup-shadow) fallbacks
    // in sync.
    static constexpr float kDefaultOffsetX = 0.0f;
    static constexpr float kDefaultOffsetY = -1.0f;
    static constexpr float kDefaultBlur = 24.0f;
    static constexpr float kDefaultOpacity = 0.25f;

    // Preference keys; the Settings rows persist through them.
    static constexpr const char* kPrefKeyDropdown = "ui.shadows.dropdown";
    static constexpr const char* kPrefKeyPicker = "ui.shadows.picker";
    static constexpr const char* kPrefKeyCompletion = "ui.shadows.completion";
    static constexpr const char* kPrefKeyTooltip = "ui.shadows.tooltip";
    static constexpr const char* kPrefKeyOffsetX = "ui.shadows.offsetX";
    static constexpr const char* kPrefKeyOffsetY = "ui.shadows.offsetY";
    static constexpr const char* kPrefKeyBlur = "ui.shadows.blur";
    static constexpr const char* kPrefKeyOpacity = "ui.shadows.opacity";

    struct Shadow
    {
        float OffsetX = kDefaultOffsetX;
        float OffsetY = kDefaultOffsetY;
        float Blur = kDefaultBlur;
        float Opacity = kDefaultOpacity;
        bool Dropdown = true;
        bool Picker = true;
        bool Completion = true;
        bool Tooltip = true;
    };

    static PopupShadowSettings& Get();

    const Shadow& GetShadow() const { return m_Shadow; }

    // Live apply only: the Settings rows persist their own field through its
    // preference key, and a slider drag re-applies on every step.
    void SetShadow(const Shadow& shadow);

    // Push the current shadow into `ui` and track it for later changes. Call
    // once per editor window after its root exists.
    void ApplyTo(UIManager* ui);

private:
    PopupShadowSettings();
    void ApplyToWindow(UIManager& ui) const;

    Shadow m_Shadow;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
