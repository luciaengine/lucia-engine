#include "Editor/Settings/AnimationSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "UI/AnimationWindowSettings.h"

#include <cstdint>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
using ColorMember = uint32_t AnimationWindowSettings::*;
using FloatMember = float AnimationWindowSettings::*;
using BoolMember = bool AnimationWindowSettings::*;

// AnimationWindowSettings persists itself; the open Animation windows redraw
// on NotifyChanged. A preview notifies without saving; a commit saves too.
void Commit()
{
    AnimationWindowSettings::Get().Save();
    AnimationWindowSettings::Get().NotifyChanged();
}

SettingsFieldDescriptor MakeColorRow(const char* label, ColorMember member)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = std::string("animation curve color colour ") + label;
    SettingsFieldDescriptor::ColorField color;
    color.DefaultArgb = AnimationWindowSettings::Defaults().*member;
    color.ShowHexField = true;
    color.Get = [member]() { return AnimationWindowSettings::Get().*member; };
    color.Preview = [member](uint32_t argb)
    {
        AnimationWindowSettings::Get().*member = argb;
        AnimationWindowSettings::Get().NotifyChanged();
    };
    color.Set = [member](uint32_t argb)
    {
        AnimationWindowSettings::Get().*member = argb;
        Commit();
    };
    field.Control = std::move(color);
    return field;
}

struct SliderRange
{
    float MinValue;
    float MaxValue;
    float Step;
};

SettingsFieldDescriptor MakeSliderRow(const char* label, const SliderRange& range, FloatMember member)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = std::string("animation ") + label;
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = AnimationWindowSettings::Defaults().*member;
    slider.MinValue = range.MinValue;
    slider.MaxValue = range.MaxValue;
    slider.Step = range.Step;
    slider.Get = [member]() { return AnimationWindowSettings::Get().*member; };
    slider.Preview = [member](float value)
    {
        AnimationWindowSettings::Get().*member = value;
        AnimationWindowSettings::Get().NotifyChanged();
    };
    slider.Set = [member](float value)
    {
        AnimationWindowSettings::Get().*member = value;
        Commit();
    };
    field.Control = std::move(slider);
    return field;
}

SettingsFieldDescriptor MakeToggleRow(const char* label, BoolMember member)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = std::string("animation ") + label;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = AnimationWindowSettings::Defaults().*member;
    toggle.Get = [member]() { return AnimationWindowSettings::Get().*member; };
    // Fired with the stored value when the page opens, which must not save.
    toggle.Set = [member](bool value)
    {
        if (AnimationWindowSettings::Get().*member == value)
            return;
        AnimationWindowSettings::Get().*member = value;
        Commit();
    };
    field.Control = std::move(toggle);
    return field;
}
} // namespace

void RegisterAnimationSettingsCategory()
{
    SettingsCategoryDescriptor animation;
    animation.CategoryId = "animation";
    animation.Title = "Animation";
    animation.Group = SettingsCategoryGroup::ProjectSettings;
    animation.TreeRowClass = "animation-row";
    animation.SearchKeywords = "animation keyframe timeline curve";

    using Settings = AnimationWindowSettings;

    SettingsFieldDescriptor keyframe = MakeColorRow("Keyframe Color", &Settings::KeyframeColor);
    keyframe.SectionHeader = "Curve Appearance";
    animation.Fields.push_back(std::move(keyframe));
    animation.Fields.push_back(MakeColorRow("Keyframe Selected Color", &Settings::KeyframeSelectedColor));
    animation.Fields.push_back(MakeColorRow("X Axis Color", &Settings::ColorX));
    animation.Fields.push_back(MakeColorRow("Y Axis Color", &Settings::ColorY));
    animation.Fields.push_back(MakeColorRow("Z Axis Color", &Settings::ColorZ));
    animation.Fields.push_back(MakeColorRow("W Axis Color", &Settings::ColorW));
    animation.Fields.push_back(MakeColorRow("Grid H-Line Color", &Settings::GridHLineColor));
    animation.Fields.push_back(MakeColorRow("Grid V-Line Color", &Settings::GridVLineColor));
    animation.Fields.push_back(MakeColorRow("Baseline Color", &Settings::BaselineColor));

    animation.Fields.push_back(MakeSliderRow("Grid Line Thickness", {0.5f, 4.0f, 0.5f}, &Settings::GridLineThickness));
    animation.Fields.push_back(MakeSliderRow("Baseline Thickness", {1.0f, 6.0f, 0.5f}, &Settings::BaselineThickness));
    animation.Fields.push_back(MakeSliderRow("Curve Line Width", {1.0f, 5.0f, 0.5f}, &Settings::CurveLineWidth));

    SettingsFieldDescriptor defaultFps = MakeSliderRow("Default FPS", {1.0f, 240.0f, 1.0f}, &Settings::DefaultFps);
    defaultFps.SectionHeader = "Timeline";
    animation.Fields.push_back(std::move(defaultFps));

    SettingsFieldDescriptor showGrid = MakeToggleRow("Show Grid", &Settings::ShowGrid);
    showGrid.SectionHeader = "Defaults";
    animation.Fields.push_back(std::move(showGrid));
    animation.Fields.push_back(MakeToggleRow("Show Bone Icons", &Settings::ShowBoneIcons));
    animation.Fields.push_back(MakeToggleRow("Snap Time", &Settings::SnapTime));
    animation.Fields.push_back(MakeToggleRow("Snap Value", &Settings::SnapValue));
    animation.Fields.push_back(MakeToggleRow("Properties Pane on Right", &Settings::PropertiesPaneOnRight));
    animation.Fields.push_back(MakeToggleRow("Ruler at Top", &Settings::RulerAtTop));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(animation));
}

} // namespace GameEngine::Editor
