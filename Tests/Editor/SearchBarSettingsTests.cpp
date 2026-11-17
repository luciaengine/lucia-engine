// The Search Bars settings rows drive the editor-wide EditorSearchBars state: Filled Search Bars puts
// "hidden" on every registered panel search bar, and Search Bars at Top moves them. The search bars
// sit at the bottom of their panels until the user picks the top.

#include <gtest/gtest.h>

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SearchBarSettingsPage.h"
#include "UI/EditorSearchBars.h"
#include "UI/PanelSearchBar.h"
#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <variant>

using GameEngine::BuildPanelSearchBar;
using GameEngine::EditorSearchBars;
using GameEngine::PanelSearchBarBuilt;
using GameEngine::Editor::EditorSettingsRegistry;
using GameEngine::Editor::RegisterSearchBarSettingsCategory;
using GameEngine::Editor::SettingsCategoryDescriptor;
using GameEngine::Editor::SettingsFieldDescriptor;

namespace
{

template <typename Control>
Control SearchBarRow(const std::string& label)
{
    RegisterSearchBarSettingsCategory();
    SettingsCategoryDescriptor category;
    EXPECT_TRUE(EditorSettingsRegistry::Get().TryGet("searchBars", category));
    for (const SettingsFieldDescriptor& field : category.Fields)
    {
        if (field.Label != label)
            continue;
        const auto* control = std::get_if<Control>(&field.Control);
        EXPECT_NE(control, nullptr) << label << " has another control type";
        return control ? *control : Control{};
    }
    ADD_FAILURE() << "the Search Bars page has no row " << label;
    return {};
}

SettingsFieldDescriptor::ToggleField SearchBarToggle(const std::string& label)
{
    return SearchBarRow<SettingsFieldDescriptor::ToggleField>(label);
}

// A leading row icon registered for the test, unregistered before it is destroyed.
struct RegisteredRowIcon
{
    GameEngine::UIElement Icon;
    RegisteredRowIcon() { EditorSearchBars::RegisterRowIcon(&Icon); }
    ~RegisteredRowIcon() { EditorSearchBars::UnregisterRowIcon(&Icon); }
};

// A panel search bar registered for the test, unregistered before it is destroyed.
struct RegisteredSearchBar
{
    PanelSearchBarBuilt Built = BuildPanelSearchBar("search-bar-settings-test-field");
    RegisteredSearchBar() { EditorSearchBars::Register(Built.RootPtr); }
    ~RegisteredSearchBar() { EditorSearchBars::Unregister(Built.RootPtr); }
};

} // namespace

TEST(SearchBarSettings, SearchBarsSitAtTheBottomByDefault)
{
    EXPECT_FALSE(SearchBarToggle("Search Bars at Top").DefaultValue);
    EXPECT_FALSE(EditorSearchBars::GetAtTop()) << "with no saved choice, the search bars sit at the top";
    const RegisteredSearchBar bar;
    EXPECT_FALSE(bar.Built.RootPtr->HasClass("search-bar-at-top"));
}

TEST(SearchBarSettings, TheFilledRowMatchesTheSavedDefault)
{
    EXPECT_FALSE(SearchBarToggle("Filled Search Bars").DefaultValue);
    EXPECT_TRUE(SearchBarToggle("Accent Search Focus Outline").DefaultValue);
}

TEST(SearchBarSettings, TheFilledRowSetsHiddenOnARegisteredPanelSearchBar)
{
    const auto toggle = SearchBarToggle("Filled Search Bars");
    ASSERT_TRUE(toggle.Set && toggle.Get);
    const bool initial = toggle.Get();
    const RegisteredSearchBar bar;

    toggle.Set(false);
    EXPECT_FALSE(EditorSearchBars::GetVisible());
    EXPECT_TRUE(bar.Built.RootPtr->HasClass("hidden"));

    toggle.Set(true);
    EXPECT_TRUE(EditorSearchBars::GetVisible());
    EXPECT_FALSE(bar.Built.RootPtr->HasClass("hidden"));

    toggle.Set(initial);
}

TEST(SearchBarSettings, TheTopRowMovesARegisteredPanelSearchBar)
{
    const auto toggle = SearchBarToggle("Search Bars at Top");
    ASSERT_TRUE(toggle.Set && toggle.Get);
    const bool initial = toggle.Get();
    const RegisteredSearchBar bar;

    toggle.Set(true);
    EXPECT_TRUE(bar.Built.RootPtr->HasClass("search-bar-at-top"));

    toggle.Set(false);
    EXPECT_FALSE(bar.Built.RootPtr->HasClass("search-bar-at-top"));

    toggle.Set(initial);
}

TEST(SearchBarSettings, TheIconAndHintAreHiddenByDefault)
{
    const auto toggle = SearchBarToggle("Show Icon and Search Hint");
    EXPECT_FALSE(toggle.DefaultValue);
    EXPECT_FALSE(EditorSearchBars::GetShowIconAndHint()) << "with no saved choice, the icon and hint show";
    const RegisteredSearchBar bar;
    const RegisteredRowIcon icon;
    EXPECT_TRUE(bar.Built.RootPtr->HasClass("search-bar-no-hint"));
    EXPECT_TRUE(icon.Icon.HasClass("search-row-icon-hidden"));
}

TEST(SearchBarSettings, TheIconAndHintRowShowsAndHidesBoth)
{
    const auto toggle = SearchBarToggle("Show Icon and Search Hint");
    ASSERT_TRUE(toggle.Set && toggle.Get);
    const bool initial = toggle.Get();
    const RegisteredSearchBar bar;
    const RegisteredRowIcon icon;

    toggle.Set(true);
    EXPECT_FALSE(bar.Built.RootPtr->HasClass("search-bar-no-hint"));
    EXPECT_FALSE(icon.Icon.HasClass("search-row-icon-hidden"));

    toggle.Set(false);
    EXPECT_TRUE(bar.Built.RootPtr->HasClass("search-bar-no-hint"));
    EXPECT_TRUE(icon.Icon.HasClass("search-row-icon-hidden"));

    toggle.Set(initial);
}

TEST(SearchBarSettings, TheStyleRowIsLuciaByDefaultAndSetsClassic)
{
    const auto style = SearchBarRow<SettingsFieldDescriptor::DropdownField>("Search Bar Style");
    ASSERT_TRUE(style.Set && style.Get);
    EXPECT_EQ(style.DefaultValue, "Lucia");
    EXPECT_EQ(style.Get(), "Lucia") << "with no saved choice, the style is not Lucia";
    const std::string initial = style.Get();
    const RegisteredSearchBar bar;
    EXPECT_FALSE(bar.Built.RootPtr->HasClass("search-bar-classic"));

    style.Set("Classic");
    EXPECT_EQ(EditorSearchBars::GetLook(), EditorSearchBars::Look::Classic);
    EXPECT_TRUE(bar.Built.RootPtr->HasClass("search-bar-classic"));

    style.Set("Lucia");
    EXPECT_FALSE(bar.Built.RootPtr->HasClass("search-bar-classic"));

    style.Set(initial);
}

TEST(SearchBarSettings, TheFilledRowShowsForTheLuciaLookOnly)
{
    RegisterSearchBarSettingsCategory();
    SettingsCategoryDescriptor category;
    ASSERT_TRUE(EditorSettingsRegistry::Get().TryGet("searchBars", category));
    std::function<bool()> filledVisible;
    for (const SettingsFieldDescriptor& field : category.Fields)
    {
        if (field.Label == "Filled Search Bars")
            filledVisible = field.IsVisible;
    }
    ASSERT_TRUE(filledVisible) << "the Filled Search Bars row shows for every look";
    const EditorSearchBars::Look initial = EditorSearchBars::GetLook();

    EditorSearchBars::SetLook(EditorSearchBars::Look::Lucia);
    EXPECT_TRUE(filledVisible());
    EditorSearchBars::SetLook(EditorSearchBars::Look::Classic);
    EXPECT_FALSE(filledVisible());

    EditorSearchBars::SetLook(initial);
}
