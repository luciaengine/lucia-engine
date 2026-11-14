#include "Editor/Settings/InputSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SceneViewSettings.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr float kLookSensitivityStep = 0.01f;
} // namespace

void RegisterInputSettingsCategory()
{
    SettingsCategoryDescriptor input;
    input.CategoryId = "input";
    input.Title = "Input";
    input.Group = SettingsCategoryGroup::ProjectSettings;
    input.TreeRowClass = "input-row";
    input.SearchKeywords = "input keyboard mouse controller";

    // SceneViewSettings persists itself, and the rows seed their Set with the
    // stored value, so each Set returns early when nothing changed.
    SettingsFieldDescriptor sensitivity;
    sensitivity.Label = "Look Sensitivity";
    sensitivity.SearchKeywords = "input mouse look sensitivity scene view camera";
    sensitivity.SectionHeader = "Scene View Mouse";
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = SceneViewSettings::kDefaultLookSensitivity;
    slider.MinValue = SceneViewSettings::kMinLookSensitivity;
    slider.MaxValue = SceneViewSettings::kMaxLookSensitivity;
    slider.Step = kLookSensitivityStep;
    slider.Get = []() { return SceneViewSettings::Get().GetLookSensitivity(); };
    slider.Set = [](float value)
    {
        if (SceneViewSettings::Get().GetLookSensitivity() == value)
            return;
        SceneViewSettings::Get().SetLookSensitivity(value);
    };
    sensitivity.Control = std::move(slider);
    input.Fields.push_back(std::move(sensitivity));

    SettingsFieldDescriptor invertY;
    invertY.Label = "Invert Y Axis";
    invertY.SearchKeywords = "input mouse invert y axis look scene view camera";
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = SceneViewSettings::kDefaultInvertY;
    toggle.Get = []() { return SceneViewSettings::Get().GetInvertY(); };
    toggle.Set = [](bool value)
    {
        if (SceneViewSettings::Get().GetInvertY() == value)
            return;
        SceneViewSettings::Get().SetInvertY(value);
    };
    invertY.Control = std::move(toggle);
    input.Fields.push_back(std::move(invertY));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(input));
}

} // namespace GameEngine::Editor
