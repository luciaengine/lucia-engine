#pragma once

namespace GameEngine::OverlayZIndex {

// z-index of transient overlays that controls attach to the root element, in
// paint order. They order within one stacking context; the OverlayLayer of an
// element decides precedence across dropdowns, modals and menus first.
inline constexpr int kDropIndicator = 9000;
inline constexpr int kDragGhost = 9400;
inline constexpr int kDragTooltip = 9500;
inline constexpr int kHoverTooltip = 9600;

} // namespace GameEngine::OverlayZIndex
