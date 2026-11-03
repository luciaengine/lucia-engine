#include "UI/Controls/ItemSizeSlider.h"

#include "Editor/Shortcuts/EditorShortcuts.h"
#include "UI/Interaction/ItemResizeGesture.h"
#include "UI/UIEvents.h"

#include <string>
#include <vector>

namespace GameEngine::EditorUI
{
namespace
{
constexpr const char* kStyleAssetPath = "UI/controls/ItemSizeSlider/ItemSizeSlider.css";

// Names the gesture's first binding, the one the Keyboard Shortcuts row lists first.
std::string TooltipFor(const std::vector<Editor::ShortcutBinding>& bindings, const ItemSizeSlider::Subject& subject)
{
    if (bindings.empty() || bindings.front().key == 0)
        return subject.Name + ". Bind Resize Items under Keyboard Shortcuts > View Navigation.";
    return subject.Name + ". " + Editor::FormatShortcutBinding(bindings.front().key, bindings.front().mods) +
        " over " + subject.Area + " or this slider also " + subject.Effect + ".";
}
} // namespace

ItemSizeSlider::ItemSizeSlider() : ItemSizeSlider(Subject{}) {}

ItemSizeSlider::ItemSizeSlider(Subject subject) : m_Subject(std::move(subject))
{
    AddClass("item-size-slider");
    SetShowValueBubble(false);
    SetTooltip(TooltipFor(Editor::ItemResizeGestureBindings(), m_Subject));
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    m_ShortcutBindingsSaved = Editor::ShortcutBindingsSaved().Subscribe(
        [this](const Editor::ShortcutCatalogEntry& entry, const std::vector<Editor::ShortcutBinding>& bindings)
        {
            if (&entry == &Editor::ItemResizeGestureCatalogEntry())
                SetTooltip(TooltipFor(bindings, m_Subject));
        });

    RegisterEventHandler(kEventScroll, [this](UIEvent& e)
    {
        if (e.ScrollY == 0.0f || !m_OnResizeGesture || !IsEnabledInHierarchy() ||
            !UI::MatchesItemResizeGesture(e.Mods))
            return;
        m_OnResizeGesture(e.ScrollY);
        e.Stop();
    });
}

} // namespace GameEngine::EditorUI
