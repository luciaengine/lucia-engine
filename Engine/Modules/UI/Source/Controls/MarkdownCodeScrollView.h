#pragma once

#include "UI/Controls/ScrollView.h"
#include "UI/UIEvents.h"

namespace GameEngine
{
// A code block scrolls sideways; vertical wheel input belongs to its surrounding page.
class MarkdownCodeScrollView final : public ScrollView
{
public:
    void OnEvent(UIEvent& event) override
    {
        if (event.Id == kEventScroll && event.ScrollX == 0.0f && event.ScrollY != 0.0f &&
            GetContentHeight() <= GetViewportHeight())
            return;
        ScrollView::OnEvent(event);
    }
};
} // namespace GameEngine
