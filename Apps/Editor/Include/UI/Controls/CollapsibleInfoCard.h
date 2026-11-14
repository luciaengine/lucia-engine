#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{
class Label;
class Button;

namespace EditorUI
{

// The editor's explanatory card. Cards read expanded; one whose text needs
// more than one row gains a chevron and can be folded into itself, which
// clamps the card to one line, cut with an ellipsis, ending in "show more" rather than
// putting a header row above it. Clicking the card toggles.
//
// OnPostLayout decides from the laid-out text whether a card folds. A one-row
// card shows no chevron glyph but keeps the chevron's box, so all copy starts at
// one x. The full text stays on a collapsed card, so a search still matches it.
//
// The chrome — chevron, copy and the "show more" action — is built in the
// constructor, so a card carries its copy as a Label in the element tree from
// the frame it is created: a tree scan, a search or a test reads the text
// without waiting on a layout pass. The folding style comes from
// UI/controls/CollapsibleInfoCard/CollapsibleInfoCard.css, requested for this
// card's own subtree.
class CollapsibleInfoCard : public UIElement
{
public:
    explicit CollapsibleInfoCard(const std::string& text);

    const std::string& GetFullText() const { return m_FullText; }
    // Replaces the copy; the card re-decides whether it needs to fold on the
    // next layout, so a card whose text shrinks to one row loses its chevron glyph.
    void SetText(const std::string& text);
    void SetAction(const std::string& label, const std::string& tooltip, std::function<void()> onInvoke);

    bool IsFoldable() const { return m_Foldable; }
    bool IsExpanded() const { return m_Expanded; }
    void SetExpanded(bool expanded);

    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;

private:
    void ApplyState();
    void SetFoldable(bool foldable);
    void UpdateToggleVisibility();

    std::string m_FullText;
    std::string m_PreviewText;
    bool m_Expanded = true;
    bool m_Foldable = false;
    bool m_FoldabilityDecided = false;
    // Owned children, built in the constructor and never replaced.
    UIElement* m_Chevron = nullptr;
    Label* m_Label = nullptr;
    // "show more", right-aligned on a collapsed card's line.
    Label* m_Toggle = nullptr;
    Button* m_Action = nullptr;
    std::function<void()> m_OnInvoke;
};

} // namespace EditorUI
} // namespace GameEngine
