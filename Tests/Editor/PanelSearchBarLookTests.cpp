// The panel search bar over the editor's own sheets: its size and inset, the empty strip it shows
// while Filled Search Bars is off, and its upright hint.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "UI/PanelSearchBar.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"

#include <memory>
#include <string>

using GameEngine::BuildPanelSearchBar;
using GameEngine::UIElement;
using InspectorLayoutTesting::FindByClass;

namespace
{

constexpr float kBarHeightPx = 22.0f;
constexpr float kBarInsetPx = 8.0f;
constexpr float kIconRestOpacity = 0.6f;
constexpr float kTolerance = 0.01f;
constexpr int kSettleFrames = 3;

struct SearchBarLook
{
    InspectorLayoutTesting::InspectorLayoutFixture Fixture;
    UIElement* Panel = nullptr;
    UIElement* Bar = nullptr;

    bool Build()
    {
        auto panel = std::make_unique<UIElement>();
        panel->AddClass("panel-search-host");
        auto built = BuildPanelSearchBar("panel-search-bar-look-test-field");
        Bar = built.RootPtr;
        panel->AddChild(std::move(built.Root));
        Panel = panel.get();
        Fixture.ExtraSheets = {"Assets/UI/theme/core.css", "Assets/UI/theme/views.css", "Assets/UI/theme/widgets.css",
                               GE_UI_MODULE_SOURCE_DIR "/Assets/UI/controls/FieldPlaceholder/FieldPlaceholder.css"};
        return Fixture.Build(std::move(panel));
    }

    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true);
    }
};

uint32_t Alpha(uint32_t argb)
{
    return argb >> 24;
}

} // namespace

TEST(PanelSearchBarLook, TheBarIs22PxHighAnd8PxAboveThePanelBottom)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    ASSERT_GT(look.Panel->GetLayoutHeight(), 0.0f) << "the panel did not lay out; this test is vacuous";
    EXPECT_NEAR(look.Bar->GetLayoutHeight(), kBarHeightPx, kTolerance);
    const float panelBottom = look.Panel->GetLayoutY() + look.Panel->GetLayoutHeight();
    const float barBottom = look.Bar->GetLayoutY() + look.Bar->GetLayoutHeight();
    EXPECT_NEAR(panelBottom - barBottom, kBarInsetPx, kTolerance);
    EXPECT_NEAR(look.Bar->GetLayoutX() - look.Panel->GetLayoutX(), kBarInsetPx, kTolerance);
}

TEST(PanelSearchBarLook, AHiddenBarStaysAsAnEmptyStripWithItsIcon)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    ASSERT_NE(Alpha(look.Bar->GetResolvedStyle().Visual.BackgroundColor), 0u)
        << "a filled bar has no fill; this test is vacuous";

    look.Bar->AddClass("hidden");
    look.Settle();
    EXPECT_NE(look.Bar->GetResolvedStyle().Layout.DisplayMode, GameEngine::DisplayMode::None)
        << "a hidden bar left the panel; the user no longer sees where to search";
    EXPECT_NEAR(look.Bar->GetLayoutHeight(), kBarHeightPx, kTolerance);
    EXPECT_EQ(Alpha(look.Bar->GetResolvedStyle().Visual.BackgroundColor), 0u);
    UIElement* icon = FindByClass(look.Bar, "panel-search-icon");
    ASSERT_NE(icon, nullptr);
    EXPECT_NE(icon->GetResolvedStyle().Layout.DisplayMode, GameEngine::DisplayMode::None);
    EXPECT_NEAR(icon->GetResolvedStyle().Visual.Opacity, kIconRestOpacity, kTolerance);
}

// The control sets "active" on focus and while a query is present: a hidden bar then shows filled.
TEST(PanelSearchBarLook, AHiddenBarFillsWhileActive)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    look.Bar->AddClass("hidden");
    look.Bar->AddClass("active");
    look.Settle();
    EXPECT_NE(Alpha(look.Bar->GetResolvedStyle().Visual.BackgroundColor), 0u);
}

TEST(PanelSearchBarLook, TheSearchHintIsUpright)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    UIElement* hint = FindByClass(look.Bar, "field-placeholder");
    ASSERT_NE(hint, nullptr) << "the panel search has no hint";
    EXPECT_EQ(hint->GetResolvedStyle().Visual.FontStyle, GameEngine::FontStyle::Normal);
}

TEST(PanelSearchBarLook, ANoHintBarHidesTheSearchHint)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    UIElement* hint = FindByClass(look.Bar, "field-placeholder");
    ASSERT_NE(hint, nullptr) << "the panel search has no hint";
    ASSERT_NE(hint->GetResolvedStyle().Layout.DisplayMode, GameEngine::DisplayMode::None)
        << "the hint is hidden without the class; this test is vacuous";

    look.Bar->AddClass("search-bar-no-hint");
    look.Bar->MarkDirtySubtree(UIElement::StyleDirty | UIElement::VisualDirty);
    look.Settle();
    EXPECT_EQ(hint->GetResolvedStyle().Layout.DisplayMode, GameEngine::DisplayMode::None);
}

TEST(PanelSearchBarLook, TheClassicLookHasAnotherFillAndABorder)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    const uint32_t luciaFill = look.Bar->GetResolvedStyle().Visual.BackgroundColor;
    const uint32_t luciaBorder = look.Bar->GetResolvedStyle().Visual.BorderColor.Top;
    EXPECT_EQ(Alpha(luciaBorder), 0u) << "the Lucia look draws a border at rest";

    look.Bar->AddClass("search-bar-classic");
    look.Settle();
    EXPECT_NE(look.Bar->GetResolvedStyle().Visual.BackgroundColor, luciaFill);
    EXPECT_NE(Alpha(look.Bar->GetResolvedStyle().Visual.BorderColor.Top), 0u)
        << "the Classic look draws no border at rest";
    EXPECT_NEAR(look.Bar->GetLayoutHeight(), kBarHeightPx, kTolerance) << "the Classic look moved the bar";
}

TEST(PanelSearchBarLook, AHiddenRowIconLeavesTheRow)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    auto buttons = std::make_unique<UIElement>();
    UIElement* group = buttons.get();
    group->AddClass("assets-toolbar-view-buttons");
    look.Panel->AddChild(std::move(buttons));
    look.Settle();
    ASSERT_EQ(group->GetResolvedStyle().Layout.DisplayMode, GameEngine::DisplayMode::Flex);

    group->AddClass("search-row-icon-hidden");
    look.Settle();
    EXPECT_EQ(group->GetResolvedStyle().Layout.DisplayMode, GameEngine::DisplayMode::None);
}

// The Classic look is always a filled field, so it reads the same whatever Filled Search Bars says.
TEST(PanelSearchBarLook, AHiddenClassicBarStaysFilled)
{
    SearchBarLook look;
    if (!look.Build())
    {
        ASSERT_EQ(look.Fixture.Diagnostic, "no Vulkan device");
        GTEST_SKIP() << look.Fixture.Diagnostic;
    }
    look.Bar->AddClass("search-bar-classic");
    look.Bar->AddClass("hidden");
    look.Settle();
    EXPECT_NE(Alpha(look.Bar->GetResolvedStyle().Visual.BackgroundColor), 0u);
    EXPECT_NE(Alpha(look.Bar->GetResolvedStyle().Visual.BorderColor.Top), 0u);
}
