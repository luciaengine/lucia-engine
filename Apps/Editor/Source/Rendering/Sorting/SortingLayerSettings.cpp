#include "Rendering/Sorting/SortingLayerSettings.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Rendering/Sorting/SortingLayerSettingsView.h"

namespace GameEngine::Editor
{
void RegisterSortingLayerSettings()
{
    SettingsCategoryDescriptor page;
    page.CategoryId = "sortingLayers";
    page.Title = "Sorting Layers";
    page.Group = SettingsCategoryGroup::ProjectSettings;
    page.Description = "Lower priorities draw first. Names and priorities can change without changing scene references. "
        "Render layers control visibility separately.";
    page.SearchKeywords = "sorting order layer mesh sprite transparency";
    page.CreateContent = []() { return std::make_unique<SortingLayerSettingsView>(); };
    EditorSettingsRegistry::Get().RegisterCategory(std::move(page));
}
}
