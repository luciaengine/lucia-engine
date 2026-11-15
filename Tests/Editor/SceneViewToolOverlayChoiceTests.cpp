// The Scene View tool mode buttons sit in the docked toolbar row unless the user
// chose the floating Tool Overlay; the choice is saved and a newly wired toolbar
// restores it.

#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/SceneViewToolbar.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>

using GameEngine::Button;
using GameEngine::kEventButtonClick;
using GameEngine::SceneViewToolbar;
using GameEngine::UIElement;
using GameEngine::UIEventHandlerAccess;
using GameEngine::UIManager;
using GameEngine::Editor::SceneViewSettings;

namespace
{

// A panel root with the toolbar (toggle and docked row) and the viewport overlay,
// owned by a real UIManager so the toolbar's wiring pass runs.
struct SceneViewToolbarFixture
{
    UIManager Manager{nullptr};
    SceneViewToolbar* Toolbar = nullptr;
    Button* Toggle = nullptr;
    UIElement* InlineButtons = nullptr;
    UIElement* Overlay = nullptr;

    SceneViewToolbarFixture()
    {
        auto root = std::make_unique<UIElement>();
        auto toolbar = std::make_unique<SceneViewToolbar>();
        Toolbar = toolbar.get();

        auto inlineButtons = std::make_unique<UIElement>();
        inlineButtons->SetId("InlineToolButtons");
        InlineButtons = inlineButtons.get();
        Toolbar->AddChild(std::move(inlineButtons));

        auto toggle = std::make_unique<Button>();
        toggle->SetId("ToolOverlayToggle");
        Toggle = toggle.get();
        Toolbar->AddChild(std::move(toggle));

        auto overlay = std::make_unique<UIElement>();
        overlay->SetId("SceneToolOverlay");
        Overlay = overlay.get();

        root->AddChild(std::move(toolbar));
        root->AddChild(std::move(overlay));
        Manager.SetRoot(std::move(root));
    }

    void Wire()
    {
        Toolbar->OnPostLayout();
        Toolbar->OnPostLayout();
    }

    bool Docked() const
    {
        return !InlineButtons->HasClass("hidden") && Overlay->HasClass("hidden") &&
               !Toggle->HasClass("icon-active");
    }

    bool Floating() const
    {
        return InlineButtons->HasClass("hidden") && !Overlay->HasClass("hidden") &&
               Toggle->HasClass("icon-active");
    }
};

} // namespace

TEST(SceneViewToolOverlayChoice, WithoutTheOverlayChoiceTheToolButtonsAreDocked)
{
    SceneViewSettings::Get().SetToolOverlayVisible(false);
    SceneViewToolbarFixture fixture;
    fixture.Wire();
    EXPECT_TRUE(fixture.Docked());
}

TEST(SceneViewToolOverlayChoice, ASavedOverlayChoiceIsRestoredWhenTheToolbarWires)
{
    SceneViewSettings::Get().SetToolOverlayVisible(true);
    SceneViewToolbarFixture fixture;
    fixture.Wire();
    EXPECT_TRUE(fixture.Floating());
    SceneViewSettings::Get().SetToolOverlayVisible(false);
}

TEST(SceneViewToolOverlayChoice, TheToggleSwitchesModesAndSavesTheChoice)
{
    SceneViewSettings::Get().SetToolOverlayVisible(false);
    SceneViewToolbarFixture fixture;
    fixture.Wire();
    ASSERT_TRUE(fixture.Docked());

    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(*fixture.Toggle, kEventButtonClick), 1u);
    fixture.Toggle->TriggerClick();
    EXPECT_TRUE(fixture.Floating());
    EXPECT_TRUE(SceneViewSettings::Get().GetToolOverlayVisible());

    SceneViewToolbarFixture reopened;
    reopened.Wire();
    EXPECT_TRUE(reopened.Floating());

    reopened.Toggle->TriggerClick();
    EXPECT_TRUE(reopened.Docked());
    EXPECT_FALSE(SceneViewSettings::Get().GetToolOverlayVisible());
}
