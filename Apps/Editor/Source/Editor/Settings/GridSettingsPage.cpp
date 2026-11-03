#include "Editor/Settings/GridSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SceneViewSettings.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
// Read at startup by GizmoSettingsWiring, which seeds the scene view with it.
constexpr const char* kPrefKeySnapSize = "grid.snapSize";

constexpr float kMinGridOpacity = 0.0f;
constexpr float kMaxGridOpacity = 1.0f;
constexpr float kGridOpacityStep = 0.05f;
constexpr float kDefaultSnapSize = 0.5f;
constexpr float kMinSnapSize = 0.1f;
constexpr float kMaxSnapSize = 10.0f;
constexpr float kSnapSizeStep = 0.1f;

using GridColorSetter = void (SceneViewSettings::*)(uint32_t, bool);
using GridColorGetter = uint32_t (SceneViewSettings::*)() const;
using GridOpacitySetter = void (SceneViewSettings::*)(float, bool);
using GridOpacityGetter = float (SceneViewSettings::*)() const;

SettingsFieldDescriptor MakeGridColorRow(const char* label, uint32_t defaultArgb, GridColorGetter get,
                                         GridColorSetter set)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = "grid color colour scene view lines";
    SettingsFieldDescriptor::ColorField color;
    color.DefaultArgb = defaultArgb;
    color.Get = [get]() { return (SceneViewSettings::Get().*get)(); };
    color.Preview = [set](uint32_t argb) { (SceneViewSettings::Get().*set)(argb, false); };
    color.Set = [set](uint32_t argb) { (SceneViewSettings::Get().*set)(argb, true); };
    field.Control = std::move(color);
    return field;
}

SettingsFieldDescriptor MakeGridOpacityRow(const char* label, float defaultValue, GridOpacityGetter get,
                                           GridOpacitySetter set)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = "grid opacity transparency scene view lines";
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = defaultValue;
    slider.MinValue = kMinGridOpacity;
    slider.MaxValue = kMaxGridOpacity;
    slider.Step = kGridOpacityStep;
    slider.Get = [get]() { return (SceneViewSettings::Get().*get)(); };
    slider.Preview = [set](float value) { (SceneViewSettings::Get().*set)(value, false); };
    slider.Set = [set](float value) { (SceneViewSettings::Get().*set)(value, true); };
    field.Control = std::move(slider);
    return field;
}
} // namespace

void RegisterGridSettingsCategory(std::function<void(float)> applySnapSize)
{
    SettingsCategoryDescriptor grid;
    grid.CategoryId = "gridSnapping";
    grid.Title = "Grid & Snapping";
    grid.Group = SettingsCategoryGroup::UserSettings;
    grid.TreeRowClass = "grid-snapping-row";
    grid.SearchKeywords = "grid snapping snap scene view";

    {
        SettingsFieldDescriptor visible;
        visible.Label = "Show Grid on Startup";
        visible.SearchKeywords = "grid show visible startup scene view";
        SettingsFieldDescriptor::ToggleField toggle;
        toggle.DefaultValue = SceneViewSettings::kDefaultGridVisibleOnStartup;
        toggle.Get = []() { return SceneViewSettings::Get().GetGridVisibleOnStartup(); };
        // Fired with the stored value when the page opens; SceneViewSettings
        // saves on every set, so an unchanged value returns early.
        toggle.Set = [](bool value)
        {
            if (SceneViewSettings::Get().GetGridVisibleOnStartup() == value)
                return;
            SceneViewSettings::Get().SetGridVisibleOnStartup(value);
        };
        visible.Control = std::move(toggle);
        grid.Fields.push_back(std::move(visible));
    }

    grid.Fields.push_back(MakeGridColorRow("Grid Color (3D)", SceneViewSettings::kDefaultGridColor3D,
                                           &SceneViewSettings::GetGridColor3D,
                                           &SceneViewSettings::SetGridColor3D));
    grid.Fields.push_back(MakeGridColorRow("Grid Color (2D)", SceneViewSettings::kDefaultGridColor2D,
                                           &SceneViewSettings::GetGridColor2D,
                                           &SceneViewSettings::SetGridColor2D));
    grid.Fields.push_back(MakeGridOpacityRow("Opacity (3D)", SceneViewSettings::kDefaultGridOpacity3D,
                                             &SceneViewSettings::GetGridOpacity3D,
                                             &SceneViewSettings::SetGridOpacity3D));
    grid.Fields.push_back(MakeGridOpacityRow("Opacity (2D)", SceneViewSettings::kDefaultGridOpacity2D,
                                             &SceneViewSettings::GetGridOpacity2D,
                                             &SceneViewSettings::SetGridOpacity2D));

    {
        // Used in 3D mode; the 2D grid adapts its spacing to the zoom.
        SettingsFieldDescriptor snapSize;
        snapSize.Label = "Snap Size";
        snapSize.SearchKeywords = "grid snap size snapping increment";
        snapSize.PrefKey = kPrefKeySnapSize;
        SettingsFieldDescriptor::SliderField slider;
        slider.DefaultValue = kDefaultSnapSize;
        slider.MinValue = kMinSnapSize;
        slider.MaxValue = kMaxSnapSize;
        slider.Step = kSnapSizeStep;
        slider.Set = std::move(applySnapSize);
        snapSize.Control = std::move(slider);
        grid.Fields.push_back(std::move(snapSize));
    }

    EditorSettingsRegistry::Get().RegisterCategory(std::move(grid));
}

} // namespace GameEngine::Editor
