#include "Editor/Settings/SceneSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
// Read by SceneEditorController, which runs the auto-save timer.
constexpr const char* kPrefKeyAutoSaveEnabled = "editor.autoSaveEnabled";
constexpr const char* kPrefKeyAutoSaveIntervalSeconds = "editor.autoSaveIntervalSeconds";

constexpr float kDefaultAutoSaveIntervalSeconds = 120.0f;
constexpr float kMinAutoSaveIntervalSeconds = 30.0f;
constexpr float kMaxAutoSaveIntervalSeconds = 300.0f;
constexpr float kAutoSaveIntervalStepSeconds = 1.0f;
} // namespace

void RegisterSceneSettingsCategory()
{
    SettingsCategoryDescriptor scene;
    scene.CategoryId = "scene";
    scene.Title = "Scene";
    scene.Group = SettingsCategoryGroup::ProjectSettings;
    scene.TreeRowClass = "scene-row";
    scene.SearchKeywords = "auto save backup scene interval seconds";
    scene.Description =
        "While editing, saves a backup copy next to your scene: <name>.backup.scene "
        "(minimum 30s, maximum 5 minutes). Requires the scene to be saved to disk at least "
        "once. Disabled during Play Mode.";

    SettingsFieldDescriptor enabled;
    enabled.Label = "Auto-save Backup Copy";
    enabled.Tooltip = "Enable auto-save (backup copy)";
    enabled.SearchKeywords = "auto save backup";
    enabled.SectionHeader = "Auto-save";
    enabled.PrefKey = kPrefKeyAutoSaveEnabled;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = true;
    enabled.Control = std::move(toggle);
    scene.Fields.push_back(std::move(enabled));

    SettingsFieldDescriptor interval;
    interval.Label = "Interval (seconds)";
    interval.SearchKeywords = "auto save interval seconds";
    interval.PrefKey = kPrefKeyAutoSaveIntervalSeconds;
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = kDefaultAutoSaveIntervalSeconds;
    slider.MinValue = kMinAutoSaveIntervalSeconds;
    slider.MaxValue = kMaxAutoSaveIntervalSeconds;
    slider.Step = kAutoSaveIntervalStepSeconds;
    interval.Control = std::move(slider);
    scene.Fields.push_back(std::move(interval));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(scene));
}

} // namespace GameEngine::Editor
