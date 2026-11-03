#pragma once

#include "UI/ResolvedStyle.h"
#include "Types/StringId.h"

#include <cstdint>

namespace GameEngine::UI {

// The accent (#3A8FFF) for code that cannot take --ui_color_accent from the
// cascade, and for hosts where no theme defines the variable.
inline constexpr uint32_t kDefaultAccentArgb = 0xFF3A8FFFu;

inline constexpr StringId kAccentColorVariable = HashStringId("--ui_color_accent");

/// The theme accent (--ui_color_accent) as resolved for an element, ARGB.
inline uint32_t ResolveAccentArgb(const ResolvedStyle& style)
{
    return style.GetCustomColor(kAccentColorVariable).value_or(kDefaultAccentArgb);
}

} // namespace GameEngine::UI
