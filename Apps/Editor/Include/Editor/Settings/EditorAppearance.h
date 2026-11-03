#pragma once

namespace GameEngine
{
class UIManager;

namespace Editor
{

// Applies every app-wide appearance setting (accent, header and row layout,
// sliders, toggles, tab icons, popup shadows, info cards, value boxes, text
// rendering) to an editor window and tracks the window, so later changes to
// any of them reach it. Call once per window after its theme is attached.
// The script line height is not included: it resolves against the window's
// font preferences, so the caller applies it after those.
void ApplyEditorAppearance(UIManager* ui);

} // namespace Editor
} // namespace GameEngine
