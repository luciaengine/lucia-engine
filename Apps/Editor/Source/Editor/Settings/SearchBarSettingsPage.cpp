#include "Editor/Settings/SearchBarSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "UI/EditorSearchBars.h"

#include <string>
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

void SetShowIconAndHint(bool show)
{
    if (show != EditorSearchBars::GetShowIconAndHint())
        EditorSearchBars::SetShowIconAndHint(show);
}

// The look names are the stored values of the dropdown.
constexpr const char* kLuciaLook = "Lucia";
constexpr const char* kClassicLook = "Classic";

std::string GetLookName()
{
    return EditorSearchBars::GetLook() == EditorSearchBars::Look::Classic ? kClassicLook : kLuciaLook;
}

void SetLookName(const std::string& name)
{
    const EditorSearchBars::Look look =
        name == kClassicLook ? EditorSearchBars::Look::Classic : EditorSearchBars::Look::Lucia;
    if (look != EditorSearchBars::GetLook())
        EditorSearchBars::SetLook(look);
}

SettingsFieldDescriptor MakeLookDropdown()
{
    SettingsFieldDescriptor field;
    field.Label = "Search Bar Style";
    field.Tooltip = "Lucia: a dark field with no border. Classic: the field fill with a subtle border "
                    "that lights up on hover.";
    field.SearchKeywords = "search bar style look classic lucia border fill";
    SettingsFieldDescriptor::DropdownField dropdown;
    dropdown.Options = {kLuciaLook, kClassicLook};
    dropdown.DefaultValue = kLuciaLook;
    dropdown.Get = &GetLookName;
    dropdown.Set = &SetLookName;
    field.Control = std::move(dropdown);
    return field;
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

    searchBars.Fields.push_back(MakeLookDropdown());
    searchBars.Fields.push_back(MakeToggle(
        "Show Icon and Search Hint",
        "Shows the icon at the left of the Hierarchy and Assets search rows, and the Search hint "
        "in an empty search field.",
        "search bar hint placeholder icon view rows show hide", false, &EditorSearchBars::GetShowIconAndHint,
        &SetShowIconAndHint));
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
