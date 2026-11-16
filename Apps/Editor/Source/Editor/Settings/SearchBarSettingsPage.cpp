#include "Editor/Settings/SearchBarSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "UI/EditorSearchBars.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{

// A row reports its initial value when the page opens; that is not an edit, so each setter
// writes only a changed value.
void SetFilled(bool filled)
{
    if (filled != EditorSearchBars::GetVisible())
        EditorSearchBars::SetVisible(filled);
}

void SetAtTop(bool atTop)
{
    if (atTop != EditorSearchBars::GetAtTop())
        EditorSearchBars::SetAtTop(atTop);
}

void SetAccentFocusOutline(bool enabled)
{
    if (enabled != EditorSearchBars::GetAccentFocusOutline())
        EditorSearchBars::SetAccentFocusOutline(enabled);
}

SettingsFieldDescriptor MakeToggle(const char* label, const char* tooltip, const char* keywords, bool defaultValue,
                                   bool (*get)(), void (*set)(bool))
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.Tooltip = tooltip;
    field.SearchKeywords = keywords;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = defaultValue;
    toggle.Get = get;
    toggle.Set = set;
    field.Control = std::move(toggle);
    return field;
}

} // namespace

void RegisterSearchBarSettingsCategory()
{
    SettingsCategoryDescriptor searchBars;
    searchBars.CategoryId = "searchBars";
    searchBars.Title = "Search Bars";
    searchBars.Group = SettingsCategoryGroup::UIAppearance;
    searchBars.SearchKeywords = "search bar field filter hierarchy assets inspector bookmarks";

    searchBars.Fields.push_back(MakeToggle(
        "Filled Search Bars",
        "Fills the search field of the Hierarchy, Assets, Inspector and Bookmarks panels. "
        "Off shows an empty strip with the search icon; the field fills when you click it or type.",
        "search bar filled background hidden show", false, &EditorSearchBars::GetVisible, &SetFilled));
    searchBars.Fields.push_back(MakeToggle("Search Bars at Top",
                                           "Puts the panel search fields at the top of their panels. "
                                           "Off puts them at the bottom.",
                                           "search bar top bottom placement position", false,
                                           &EditorSearchBars::GetAtTop, &SetAtTop));
    searchBars.Fields.push_back(MakeToggle("Accent Search Focus Outline",
                                           "Draws the focused search field's outline in the accent color.",
                                           "search bar focus outline accent color", true,
                                           &EditorSearchBars::GetAccentFocusOutline, &SetAccentFocusOutline));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(searchBars));
}

} // namespace GameEngine::Editor
