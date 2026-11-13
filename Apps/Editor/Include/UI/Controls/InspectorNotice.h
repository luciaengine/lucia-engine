#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{
class Button;
class Label;

namespace EditorUI
{

// A notice in an inspector: one statement the user has to read before trusting the fields around
// it, optionally the one action that resolves it (discard a preserved value, select the entity
// that generates this one), and optionally a Dismiss button for a notice the user may put away.
//
// The plate, the text, the action and the dismiss are declared in
// UI/controls/InspectorNotice/InspectorNotice.uxml and styled by InspectorNotice.css beside it. The
// layout is instantiated in the constructor, not when the notice gains a UIManager, so the text and
// the buttons are in the element tree from the frame the notice is built: a tree scan, a search or a
// test reads them without a layout pass. The buttons start hidden; SetAction and SetDismiss show them.
//
// A warning (the default) says something is wrong or about to be lost and is drawn in the warning
// palette; an information notice states how the fields around it behave and is drawn neutral; an
// error notice says something failed. The kind is one class on the control
// (inspector-notice-<kind>) and one on each part (inspector-notice-<part>-<kind>: the plate, the
// glyph, the title, the text, the action and the dismiss), which InspectorNotice.css styles.
class InspectorNotice : public UIElement
{
public:
    enum class Kind
    {
        Warning,
        Information,
        // Something failed: the error text colour on a red wash.
        Error,
    };

    // The heading's glyph: the kind's own, a clock for a wait, or a check mark for a success.
    enum class Glyph
    {
        OfKind,
        Clock,
        Check,
    };

    explicit InspectorNotice(const std::string& text, Kind kind = Kind::Warning);

    // Shows the action button with `label` and `tooltip`; a click calls `onInvoke`.
    void SetAction(const std::string& label, const std::string& tooltip, std::function<void()> onInvoke);

    // Shows the Dismiss button, after the action, with `tooltip`; a click calls `onDismiss`. The owner
    // decides what dismissing means and how long it lasts, and hides the notice itself.
    void SetDismiss(const std::string& tooltip, std::function<void()> onDismiss);

    // Opens the notice with a heading: the kind's glyph and `title`, above the text.
    void SetTitle(const std::string& title);

    // Draws `glyph` in the heading, in the kind's color.
    void SetGlyph(Glyph glyph);

    // Replaces the notice's text.
    void SetText(const std::string& text);

private:
    void InvokeAction();
    void InvokeDismiss();

    // Owned children, built in the constructor from the layout and never replaced. Null only when
    // the layout did not load, which is logged.
    Label* m_Text = nullptr;
    Button* m_Action = nullptr;
    Button* m_Dismiss = nullptr;
    UIElement* m_Heading = nullptr;
    Label* m_Title = nullptr;
    UIElement* m_Glyph = nullptr;
    std::function<void()> m_OnInvoke;
    std::function<void()> m_OnDismiss;
};

} // namespace EditorUI
} // namespace GameEngine
