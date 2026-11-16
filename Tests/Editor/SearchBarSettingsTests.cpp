// The Search Bars settings rows drive the editor-wide EditorSearchBars state: Filled Search Bars puts
// "hidden" on every registered panel search bar, and Search Bars at Top moves them. The search bars
// sit at the bottom of their panels until the user picks the top.

#include <gtest/gtest.h>

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SearchBarSettingsPage.h"
#include "UI/EditorSearchBars.h"
#include "UI/PanelSearchBar.h"

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

SettingsFieldDescriptor::ToggleField SearchBarToggle(const std::string& label)
{
    RegisterSearchBarSettingsCategory();
    SettingsCategoryDescriptor category;
    EXPECT_TRUE(EditorSettingsRegistry::Get().TryGet("searchBars", category));
    for (const SettingsFieldDescriptor& field : category.Fields)
    {
        if (field.Label != label)
            continue;
        const auto* toggle = std::get_if<SettingsFieldDescriptor::ToggleField>(&field.Control);
        EXPECT_NE(toggle, nullptr) << label << " is not a toggle";
        return toggle ? *toggle : SettingsFieldDescriptor::ToggleField{};
    }
    ADD_FAILURE() << "the Search Bars page has no row " << label;
    return {};
}

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
