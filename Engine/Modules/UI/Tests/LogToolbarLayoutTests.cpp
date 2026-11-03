#include "IsolatedUIFixture.h"

#include "Core/Application.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TextField.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
std::string ReadLogToolbarAsset(const char* relative)
{
    const auto path = GameEngine::PathUtils::GetExecutableDirectory() / "Assets/UI" / relative;
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string LogToolbarStyle()
{
    return ReadLogToolbarAsset("theme/tokens.css") + "\n" +
           ReadLogToolbarAsset("controls/Button.css") + "\n" +
           ReadLogToolbarAsset("theme/widgets.css") + "\n" +
           ReadLogToolbarAsset("panels/LogPanel.css");
}

GameEngine::SearchFieldWithFilter* AddLogSearch(GameEngine::UITesting::IsolatedUIFixture& fixture)
{
    using namespace GameEngine;
    auto ownedSearch = std::make_unique<SearchFieldWithFilter>();
    auto* search = ownedSearch.get();
    search->SetId("search");
    search->AddClass("panel-search-bar");
    search->AddClass("log-toolbar-search");
    fixture.Element("row")->AddChild(std::move(ownedSearch));
    search->SetFilterOptions({{"all", "All fields"}, {"message", "Message"}});
    search->AddClass("active");
    search->GetField()->AddClass("log-toolbar-search-input");
    search->GetFilter()->SetId("filter");
    search->GetSearchIcon()->SetId("search-icon");
    return search;
}
}

TEST(LogToolbarLayout, NarrowSearchKeepsItsFilterAndSearchIconInsideTheRow)
{
    using namespace GameEngine;
    UITesting::IsolatedUIFixture fixture;
    const std::string css = LogToolbarStyle() + R"(
#root { width: 220px; height: 60px; display: flex; flex-direction: column; }
#actions { width: 100px; height: 24px; flex-shrink: 0; }
)";
    const bool built = fixture.Build(1.0f, R"(<UIElement id="root">
<UIElement id="row" class="log-toolbar">
<UIElement id="actions"/>
</UIElement></UIElement>)", css);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    AddLogSearch(fixture);
    fixture.Settle();
    const auto row = fixture.BorderBox("row");
    const auto field = fixture.BorderBox("search");
    const auto filter = fixture.BorderBox("filter");
    const auto icon = fixture.BorderBox("search-icon");
    EXPECT_LT(field.W, 160.0f) << "The fixture must exercise the narrow search layout";
    EXPECT_GE(field.W, 88.0f);
    EXPECT_LE(field.X + field.W, row.X + row.W);
    EXPECT_GE(filter.X, field.X);
    EXPECT_LE(filter.X + filter.W, field.X + field.W);
    EXPECT_GE(icon.X, filter.X + filter.W);
    EXPECT_LE(icon.X + icon.W, row.X + row.W);
}

TEST(LogToolbarLayout, ActionIconsAreCenteredAndClearFollowsCopy)
{
    using namespace GameEngine;
    UITesting::IsolatedUIFixture fixture;
    const auto layout = ReadLogToolbarAsset("controls/LogToolbarActions.uxml");
    ASSERT_FALSE(layout.empty());
    const bool built = fixture.Build(1.0f, "<UIElement id=\"root\">" + layout + "</UIElement>",
                                    LogToolbarStyle() + "\n#root { width: 400px; height: 60px; }");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    const auto pause = fixture.BorderBox("LogPause");
    const auto copy = fixture.BorderBox("LogCopy");
    const auto clear = fixture.BorderBox("LogClear");
    const auto icon = fixture.BorderBox("LogPauseIcon");
    EXPECT_LT(pause.X, copy.X);
    EXPECT_LT(copy.X, clear.X);
    EXPECT_FLOAT_EQ(pause.W, 80.0f);
    EXPECT_FLOAT_EQ(copy.W, pause.W);
    EXPECT_FLOAT_EQ(clear.W, pause.W);
    EXPECT_FLOAT_EQ(icon.W, 14.0f);
    EXPECT_FLOAT_EQ(icon.Y + icon.H * 0.5f, pause.Y + pause.H * 0.5f);
}

TEST(LogToolbarLayout, MinimumSearchWidthLeavesSpaceForTheTextCaret)
{
    using namespace GameEngine;
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, R"(<UIElement id="root">
<UIElement id="row" class="log-toolbar"><UIElement id="actions"/></UIElement>
</UIElement>)", LogToolbarStyle() + R"(
#root { width: 212px; height: 60px; }
#actions { width: 100px; height: 24px; flex-shrink: 0; }
#search { width: 72px; }
)");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* search = AddLogSearch(fixture);
    fixture.Settle();
    TextInput* input = nullptr;
    for (const auto& child : search->GetField()->GetChildren())
    {
        if (auto* textInput = dynamic_cast<TextInput*>(child.get()))
        {
            input = textInput;
            break;
        }
    }
    ASSERT_NE(input, nullptr);
    const auto& padding = input->GetResolvedStyle().Layout.Padding;
    EXPECT_GE(input->GetLayoutWidth() - padding.Left - padding.Right, 1.0f)
        << "The text caret needs content space after both input and trailing-control padding";
    const auto row = fixture.BorderBox("row");
    const auto icon = fixture.BorderBox("search-icon");
    EXPECT_LE(icon.X + icon.W, row.X + row.W);
}

TEST(LogToolbarLayout, CompactRowWrapsSearchInsteadOfClippingItsControls)
{
    using namespace GameEngine;
    UITesting::IsolatedUIFixture fixture;
    const auto actions = ReadLogToolbarAsset("controls/LogToolbarActions.uxml");
    ASSERT_FALSE(actions.empty());
    const bool built = fixture.Build(1.0f,
        "<UIElement id=\"root\"><UIElement id=\"row\" class=\"log-toolbar log-toolbar-secondary\">" +
        actions + "</UIElement></UIElement>",
        LogToolbarStyle() + "\n#root { width: 360px; height: 120px; }");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    AddLogSearch(fixture)->AddClass("log-toolbar-search-compact");
    fixture.Settle();
    const auto row = fixture.BorderBox("row");
    const auto search = fixture.BorderBox("search");
    const auto copy = fixture.BorderBox("LogCopy");
    const auto icon = fixture.BorderBox("search-icon");
    EXPECT_GE(search.Y, copy.Y + copy.H);
    EXPECT_GE(search.X, row.X);
    EXPECT_LE(icon.X + icon.W, row.X + row.W);
    EXPECT_LE(search.Y + search.H, row.Y + row.H);
}

TEST(LogToolbarLayout, MediumCompactRowShrinksSearchBeforeWrapping)
{
    using namespace GameEngine;
    UITesting::IsolatedUIFixture fixture;
    const auto actions = ReadLogToolbarAsset("controls/LogToolbarActions.uxml");
    ASSERT_FALSE(actions.empty());
    const bool built = fixture.Build(1.0f,
        "<UIElement id=\"root\"><UIElement id=\"row\" class=\"log-toolbar log-toolbar-secondary\">"
        "<UIElement class=\"log-toolbar-center-slot\">" + actions + "</UIElement></UIElement></UIElement>",
        LogToolbarStyle() + "\n#root { width: 600px; height: 120px; }");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    AddLogSearch(fixture)->AddClass("log-toolbar-search-compact");
    fixture.Settle();
    const auto search = fixture.BorderBox("search");
    const auto copy = fixture.BorderBox("LogCopy");
    EXPECT_FLOAT_EQ(search.Y + search.H * 0.5f, copy.Y + copy.H * 0.5f);
    EXPECT_GE(search.W, 88.0f);
    EXPECT_LT(search.W, 320.0f);
}

TEST(LogToolbarLayout, WideToolbarHidesItsUnusedSecondaryRow)
{
    using namespace GameEngine;
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, R"(<UIElement id="root">
<UIElement id="primary" class="log-toolbar" />
<UIElement id="secondary" class="log-toolbar log-toolbar-secondary log-toolbar-secondary-hidden" />
</UIElement>)", LogToolbarStyle() +
        "\n#root { width: 1400px; height: 120px; display: flex; flex-direction: column; }");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    EXPECT_GE(fixture.BorderBox("primary").H, 32.0f);
    EXPECT_FLOAT_EQ(fixture.BorderBox("secondary").H, 0.0f);
}
