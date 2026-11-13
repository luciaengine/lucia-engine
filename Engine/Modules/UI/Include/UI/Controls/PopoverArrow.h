#pragma once

#include "UI/UIElement.h"

#include <cstdint>

namespace GameEngine
{
namespace UI
{
enum class ArrowDirection : uint8_t;
}

// The arrow of a popup, a child of the bubble it points from, painted with the bubble's
// background and border. Its tip is at the middle of its box's far edge on `side`; its base
// is on the bubble's edge. Purely visual; it takes no pointer events.
class PopoverArrow : public UIElement
{
public:
    PopoverArrow();

    // The direction the tip points: up (the default), down, left or right.
    void SetSide(UI::ArrowDirection side);

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float W, float H) override;

private:
    UI::ArrowDirection m_Side{}; // Up
};

} // namespace GameEngine
