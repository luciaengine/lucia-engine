// The editor's explanatory card, as everything that reads a UI tree sees it.
//
// The card is a control that OWNS the Label carrying its copy, so a card's text is not a child of
// whatever added it. That makes one property load-bearing for every consumer that is not the
// renderer — search, a tree dump, a notice test: the copy has to be in the tree from the frame the
// card is built, not from the frame it is first laid out. Chrome that appears only after a layout
// pass reads as an empty box to all of them, and reads as "the notice was never added" to a test.
//
// Folding stays a layout decision (OnPostLayout measures the laid-out copy against one row), which
// is why the chevron and the action are asserted here only as present-and-quiet.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "InspectorLayoutFixture.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/InfoCard.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

using GameEngine::Label;
using GameEngine::UIElement;
using GameEngine::EditorUI::CollapsibleInfoCard;
using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

std::vector<std::string> AllLabelTextIn(const UIElement& parent)
{
    std::vector<std::string> texts;
    for (const auto& child : parent.GetChildren())
    {
        if (!child)
            continue;
        if (const auto* label = dynamic_cast<const Label*>(child.get()))
            texts.push_back(label->GetText());
        for (std::string& nested : AllLabelTextIn(*child))
            texts.push_back(std::move(nested));
    }
    return texts;
}

bool HoldsText(const UIElement& parent, const std::string& text)
{
    for (const std::string& candidate : AllLabelTextIn(parent))
        if (candidate == text)
            return true;
    return false;
}

} // namespace

// The property the notice tests rest on: no UIManager, no layout pass, and the copy is still
// there to be read.
TEST(CollapsibleInfoCard, CarriesItsCopyAsALabelFromConstruction)
{
    const std::string copy = "Region is every terrain - no edge, no falloff.";
    CollapsibleInfoCard card(copy);

    EXPECT_EQ(card.GetFullText(), copy);
    EXPECT_TRUE(HoldsText(card, copy)) << "the card's copy is not in the element tree";
}

// A card added to a host is readable through the host, which is how every notice site reaches it.
TEST(CollapsibleInfoCard, ItsCopyIsReachableThroughTheHostThatAddedIt)
{
    UIElement host;
    const std::string copy = "Add Rule below, then give the rule a condition.";
    host.AddChild(std::make_unique<CollapsibleInfoCard>(copy));

    EXPECT_TRUE(HoldsText(host, copy));
}

// Replacing the copy replaces what the tree carries, so a card that is rebuilt in place cannot
// leave the previous material's sentence behind it.
TEST(CollapsibleInfoCard, SetTextReplacesTheCopyInTheTree)
{
    CollapsibleInfoCard card("The first thing it said.");
    card.SetText("The second thing it said.");

    // The new copy first: it is what makes the absence below mean "replaced" rather than "the
    // tree is empty", which an empty tree would satisfy on its own.
    ASSERT_TRUE(HoldsText(card, "The second thing it said."));
    EXPECT_FALSE(HoldsText(card, "The first thing it said."));
}

// Cards read expanded, and one that was never laid out was never measured — so it has made no
// fold decision and offers no action to undo one.
TEST(CollapsibleInfoCard, AnUnmeasuredCardIsExpandedAndNotFoldable)
{
    CollapsibleInfoCard card("A sentence. And a second one, which the preview would cut.");

    EXPECT_TRUE(card.IsExpanded());
    EXPECT_FALSE(card.IsFoldable());
    // Expanded means the whole text, not the preview the first sentence would give.
    EXPECT_TRUE(HoldsText(card, "A sentence. And a second one, which the preview would cut."));
}

// A card whose copy changes while it is on screen (a live stats line) keeps its chevron until a
// measurement of the new copy says otherwise: dropping it on every change would hide the chevron
// for a frame, and with it the width and the wrapping it takes.
TEST(CollapsibleInfoCardFold, NewCopyKeepsTheChevronUntilItIsMeasured)
{
    const std::string twoRows =
        "Playing - Particles 12 - Preview 3.5 s. Simulation 0.4 ms - Ticks 210 - Catch-up dropped 0.0 s, "
        "so the line is long enough to wrap onto a second row at this width.";
    const std::string otherTwoRows =
        "Playing - Particles 18 - Preview 7.7 s. Simulation 0.6 ms - Ticks 462 - Catch-up dropped 0.0 s, "
        "so the line is long enough to wrap onto a second row at this width.";
    auto root = std::make_unique<UIElement>();
    auto card = std::make_unique<CollapsibleInfoCard>(twoRows);
    CollapsibleInfoCard* cardRaw = card.get();
    root->AddChild(std::move(card));

    InspectorLayoutFixture fixture;
    fixture.ViewportW = 360;
    fixture.ExtraSheets = {"Assets/UI/theme/core.css", "Assets/UI/controls/CollapsibleInfoCard/CollapsibleInfoCard.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    UIElement* chevron = FindByClass(cardRaw, "editor-info-card-chevron");
    ASSERT_NE(chevron, nullptr);
    ASSERT_TRUE(cardRaw->IsFoldable()) << "the long copy did not wrap, so the fixture cannot show a fold";

    cardRaw->SetText(otherTwoRows);
    EXPECT_TRUE(cardRaw->IsFoldable()) << "new copy dropped the fold state before it was measured";
    EXPECT_FALSE(chevron->HasClass("hidden")) << "new copy hid the chevron before it was measured";

    for (int frame = 0; frame < 3; ++frame)
        fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
    EXPECT_TRUE(cardRaw->IsFoldable());

    cardRaw->SetText("Short.");
    for (int frame = 0; frame < 3; ++frame)
        fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
    EXPECT_FALSE(cardRaw->IsFoldable()) << "the measurement of one row still ends the fold";
    EXPECT_TRUE(chevron->HasClass("hidden"));
}

// A card's action stays hidden until the card is given one; a click on it runs the action and never
// folds the card.
TEST(CollapsibleInfoCard, SetActionShowsTheButtonAndAClickRunsIt)
{
    CollapsibleInfoCard card("Sky Environment sets this light's direction and colour from the time of day.");
    auto* action = dynamic_cast<GameEngine::Button*>(FindByClass(&card, "editor-info-card-action"));
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->HasClass("hidden"));

    int invoked = 0;
    card.SetAction("Select sky", "Select the sky.", [&invoked]() { ++invoked; });
    EXPECT_FALSE(action->HasClass("hidden"));
    EXPECT_EQ(action->GetText(), "Select sky");

    action->TriggerClick();
    EXPECT_EQ(invoked, 1);
    EXPECT_TRUE(card.IsExpanded());
}

// A left release on a measured, foldable card's action presses the action and leaves the fold alone,
// while the same release on the card's own row still folds it. The card sees the release as it
// bubbles up from the action, so it is dispatched to the card with the action as its target.
TEST(CollapsibleInfoCardFold, AReleaseOnTheActionRunsItAndNeverFoldsTheCard)
{
    const std::string twoRows =
        "Sky Environment sets this light's direction and colour from the time of day, so the line is "
        "long enough to wrap onto a second row at this width beside the action.";
    auto root = std::make_unique<UIElement>();
    auto card = std::make_unique<CollapsibleInfoCard>(twoRows);
    int invoked = 0;
    card->SetAction("Select sky", "Select the sky.", [&invoked]() { ++invoked; });
    CollapsibleInfoCard* cardRaw = card.get();
    root->AddChild(std::move(card));

    InspectorLayoutFixture fixture;
    fixture.ViewportW = 360;
    fixture.ExtraSheets = {"Assets/UI/theme/core.css", "Assets/UI/controls/CollapsibleInfoCard/CollapsibleInfoCard.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    auto* action = dynamic_cast<GameEngine::Button*>(FindByClass(cardRaw, "editor-info-card-action"));
    ASSERT_NE(action, nullptr);
    ASSERT_TRUE(cardRaw->IsFoldable()) << "the long copy did not wrap, so the fixture cannot show a fold";
    ASSERT_GT(action->GetLayoutWidth(), 0.0f);
    ASSERT_GT(action->GetLayoutHeight(), 0.0f);

    const float x = action->GetLayoutX() + action->GetLayoutWidth() * 0.5f;
    const float y = action->GetLayoutY() + action->GetLayoutHeight() * 0.5f;
    auto mouse = [x, y](GameEngine::EventId id, UIElement* target, UIElement* current) {
        GameEngine::UIEvent e{};
        e.Id = id;
        e.Button = 0;
        e.X = x;
        e.Y = y;
        e.Target = target;
        e.CurrentTarget = current;
        return e;
    };

    const bool expanded = cardRaw->IsExpanded();
    GameEngine::UIEvent press = mouse(GameEngine::kEventMouseDown, action, action);
    action->DispatchEvent(press);
    GameEngine::UIEvent release = mouse(GameEngine::kEventMouseUp, action, action);
    action->DispatchEvent(release);
    GameEngine::UIEvent bubbled = mouse(GameEngine::kEventMouseUp, action, cardRaw);
    cardRaw->DispatchEvent(bubbled);
    EXPECT_EQ(invoked, 1) << "the release on the action did not run it";
    EXPECT_EQ(cardRaw->IsExpanded(), expanded) << "a release on the action folded the card";

    GameEngine::UIEvent onCard = mouse(GameEngine::kEventMouseUp, cardRaw, cardRaw);
    cardRaw->DispatchEvent(onCard);
    EXPECT_NE(cardRaw->IsExpanded(), expanded) << "a release on the card's own row no longer folds it";
    EXPECT_EQ(invoked, 1);
}

// With "Show Info Cards" off, a card with an action keeps its action on screen and drops its copy, so
// the state the action names stays visible; a card without one is gone.
TEST(CollapsibleInfoCardFold, AHiddenCardKeepsItsActionAndDropsItsCopy)
{
    auto root = std::make_unique<UIElement>();
    auto withAction = std::make_unique<CollapsibleInfoCard>("Sky Environment sets this light's direction.");
    withAction->SetAction("Select sky", "Select the sky.", []() {});
    withAction->AddClass(GameEngine::EditorUI::kInfoCardOffClass);
    CollapsibleInfoCard* withActionRaw = withAction.get();
    auto plain = std::make_unique<CollapsibleInfoCard>("An explanation and nothing else.");
    plain->AddClass(GameEngine::EditorUI::kInfoCardOffClass);
    CollapsibleInfoCard* plainRaw = plain.get();
    root->AddChild(std::move(withAction));
    root->AddChild(std::move(plain));

    InspectorLayoutFixture fixture;
    fixture.ViewportW = 360;
    fixture.ExtraSheets = {"Assets/UI/theme/core.css", "Assets/UI/controls/CollapsibleInfoCard/CollapsibleInfoCard.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    UIElement* action = FindByClass(withActionRaw, "editor-info-card-action");
    UIElement* line = FindByClass(withActionRaw, "editor-info-card-line");
    ASSERT_NE(action, nullptr);
    ASSERT_NE(line, nullptr);

    EXPECT_GT(action->GetLayoutWidth(), 0.0f) << "the hidden card dropped its action";
    EXPECT_GT(action->GetLayoutHeight(), 0.0f);
    EXPECT_EQ(line->GetLayoutHeight(), 0.0f) << "the hidden card still shows its copy";
    EXPECT_EQ(plainRaw->GetLayoutHeight(), 0.0f) << "a hidden card without an action still takes space";
}
