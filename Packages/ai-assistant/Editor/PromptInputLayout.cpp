#include "PromptInputLayout.h"

#include "UI/UIElement.h"

#include <utility>

namespace GameEngine
{
void ArrangePromptInput(UIElement& row, UIElement& controls, UIElement& attach, bool stacked)
{
    // OnPostLayout runs inside the manager's dispatch guard; TakeChild is refused there.
    if (UIElement::IsInEventDispatch())
    {
        row.PostSafeAction([&row, controlsRef = UIElement::MakeWeakRef(&controls),
                            attachRef = UIElement::MakeWeakRef(&attach), stacked]()
        {
            if (controlsRef.Get() && attachRef.Get())
                ArrangePromptInput(row, *controlsRef.Get(), *attachRef.Get(), stacked);
        });
        return;
    }
    UIElement* destination = stacked ? &controls : &row;
    if (attach.GetParent() != destination)
        destination->InsertChild(0, attach.GetParent()->TakeChild(&attach));
    if (stacked)
    {
        row.AddClass("prompt-panel-input-row-stacked");
        controls.AddClass("prompt-panel-input-controls-stacked");
    }
    else
    {
        row.RemoveClass("prompt-panel-input-row-stacked");
        controls.RemoveClass("prompt-panel-input-controls-stacked");
    }
}
} // namespace GameEngine
