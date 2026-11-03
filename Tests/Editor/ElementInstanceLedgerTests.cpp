// ElementInstanceLedger: the record a panel re-binding after a .uxml reconcile uses to act
// only on element instances it has not set up before.
//
// A reconcile keeps an unchanged element — same object, same handler table — and replaces
// a changed one with a new, unwired instance. The Animation window re-runs its whole
// binding on every reconcile announcement, so these arms pin the two answers that make
// that safe: a kept button is not wired a second time (one click fires once), and a
// replacement is wired. The third arm pins the pass rule: two blocks of one pass that
// set up the same new element both get it.

#include <gtest/gtest.h>

#include "UI/Controls/Button.h"
#include "UI/ElementInstanceLedger.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <memory>

using GameEngine::Button;
using GameEngine::UIElement;
using GameEngine::UIEvent;
using GameEngine::Editor::ElementInstanceLedger;

namespace
{
// One binding pass over one button, the shape of a wiring block in the panel.
void WirePass(ElementInstanceLedger& ledger, Button* button, int& clicks)
{
    if (ledger.IsNew(button))
        button->RegisterEventHandler(GameEngine::kEventButtonClick, [&clicks](UIEvent&) { ++clicks; });
    ledger.EndPass();
}

void Click(Button* button)
{
    UIEvent click{};
    click.Id = GameEngine::kEventButtonClick;
    click.Target = button;
    click.CurrentTarget = button;
    button->DispatchEvent(click);
}
} // namespace

TEST(ElementInstanceLedgerTests, AKeptElementIsWiredOnceAcrossPasses)
{
    ElementInstanceLedger ledger;
    auto button = std::make_unique<Button>();
    int clicks = 0;

    WirePass(ledger, button.get(), clicks);
    // A reconcile that kept the button re-runs the pass over the same instance.
    WirePass(ledger, button.get(), clicks);

    Click(button.get());
    EXPECT_EQ(clicks, 1) << "the kept button was wired again and fires once per pass";
}

TEST(ElementInstanceLedgerTests, AReplacementElementIsWired)
{
    ElementInstanceLedger ledger;
    auto original = std::make_unique<Button>();
    int originalClicks = 0;
    WirePass(ledger, original.get(), originalClicks);
    original.reset();

    // The reconcile destroyed the button and created a new one with the same id.
    auto replacement = std::make_unique<Button>();
    int replacementClicks = 0;
    WirePass(ledger, replacement.get(), replacementClicks);

    Click(replacement.get());
    EXPECT_EQ(replacementClicks, 1) << "the replacement was never wired";
}

TEST(ElementInstanceLedgerTests, TwoBlocksOfOnePassBothSeeANewElement)
{
    ElementInstanceLedger ledger;
    auto element = std::make_unique<UIElement>();

    EXPECT_TRUE(ledger.IsNew(element.get()));
    EXPECT_TRUE(ledger.IsNew(element.get())) << "a second block of the same pass lost the new element";
    ledger.EndPass();
    EXPECT_FALSE(ledger.IsNew(element.get())) << "EndPass did not record the pass";
    EXPECT_FALSE(ledger.IsNew(nullptr));
}
