#pragma once

namespace GameEngine
{
class UIElement;

/// Places the existing file picker before the prompt controls when they sit below the
/// text field, and returns it to the field's left when the row is wide again.
void ArrangePromptInput(UIElement& row, UIElement& controls, UIElement& attach, bool stacked);
} // namespace GameEngine
