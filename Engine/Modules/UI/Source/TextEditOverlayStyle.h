#pragma once

#include "UI/ResolvedStyle.h"
#include "UI/ThemeAccent.h"

#include <cstdint>

namespace GameEngine::UI {

// Width of the caret bar in CSS-logical px; callers scale it to physical px.
inline constexpr float kCaretThicknessLogicalPx = 1.5f;

inline constexpr uint32_t kSelectionAlphaFocused = 0x66u;
inline constexpr uint32_t kSelectionAlphaUnfocused = 0x44u;

/// ARGB fill of the text selection highlight shared by TextInput and TextArea:
/// the element's border-top colour (or the default accent) with a fixed alpha
/// that dims while the element does not have focus.
inline uint32_t SelectionFillArgb(const VisualStyle& visual, bool focused)
{
    uint32_t rgb = visual.BorderColor.Top;
    if ((rgb >> 24) == 0)
        rgb = kDefaultAccentArgb;
    const uint32_t alpha = focused ? kSelectionAlphaFocused : kSelectionAlphaUnfocused;
    return (alpha << 24) | (rgb & 0x00FFFFFFu);
}

} // namespace GameEngine::UI
