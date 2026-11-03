#include "Editor/Settings/GizmoSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "SceneView/TransformTool.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
using SceneTools::TransformTool;
using TransformToolAccessor = std::function<TransformTool*()>;
using ToolFloatSetter = void (TransformTool::*)(float);
using LightDetailsGetter = bool (SceneViewSettings::*)() const;
using LightDetailsSetter = void (SceneViewSettings::*)(bool);

constexpr const char* kPrefKeyConstantScreenSize = "gizmo.constantScreenSize";
constexpr const char* kPrefKeyRotateEnhanced = "gizmo.rotateEnhanced";
constexpr bool kDefaultConstantScreenSize = true;
constexpr bool kDefaultRotateEnhanced = false;

constexpr float kMinThickness = 0.5f;
constexpr float kThicknessStep = 0.1f;
constexpr float kMinScale = 0.25f;
constexpr float kMaxScale = 3.0f;
constexpr float kScaleStep = 0.05f;

// One size slider. Constant Screen Size picks which set applies: pixel
// thickness while it is off; a scale multiplier and its own thickness while it
// is on. The rows of the other set are hidden.
struct GizmoSliderRow
{
    const char* Label;
    const char* PrefKey;
    const char* SectionHeader; // nullptr continues the section above
    float DefaultValue;
    float MinValue;
    float MaxValue;
    float Step;
    bool ShownWithConstantScreenSize;
    ToolFloatSetter Apply;
};

constexpr std::array<GizmoSliderRow, 9> kSliderRows = {{
    {"Translate", "gizmo.translateThickness", "Gizmo Thickness", 1.5f, kMinThickness, 3.0f,
     kThicknessStep, false, &TransformTool::SetTranslateGizmoThickness},
    {"Rotate", "gizmo.rotateThickness", nullptr, 5.5f, kMinThickness, 10.0f, kThicknessStep, false,
     &TransformTool::SetRotateGizmoThickness},
    {"Scale", "gizmo.scaleThickness", nullptr, 1.5f, kMinThickness, 5.0f, kThicknessStep, false,
     &TransformTool::SetScaleGizmoThickness},
    {"Translate", "gizmo.translateScale", "Gizmo Scale", 1.0f, kMinScale, kMaxScale, kScaleStep, true,
     &TransformTool::SetTranslateGizmoScale},
    {"Rotate", "gizmo.rotateScale", nullptr, 1.5f, kMinScale, kMaxScale, kScaleStep, true,
     &TransformTool::SetRotateGizmoScale},
    {"Scale", "gizmo.scaleGizmoScale", nullptr, 1.0f, kMinScale, kMaxScale, kScaleStep, true,
     &TransformTool::SetScaleGizmoScale},
    {"Translate", "gizmo.translateConstantThickness", "Gizmo Thickness", 0.8f, kMinThickness, 5.0f,
     kThicknessStep, true, &TransformTool::SetTranslateConstantThickness},
    {"Rotate", "gizmo.rotateConstantThickness", nullptr, 4.2f, kMinThickness, 8.0f, kThicknessStep, true,
     &TransformTool::SetRotateConstantThickness},
    {"Scale", "gizmo.scaleConstantThickness", nullptr, 1.5f, kMinThickness, 5.0f, kThicknessStep, true,
     &TransformTool::SetScaleConstantThickness},
}};

SettingsFieldDescriptor MakeToolToggleRow(const char* label, const char* prefKey, bool defaultValue,
                                          std::function<void(bool)> set)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = std::string("gizmo ") + label;
    field.PrefKey = prefKey;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = defaultValue;
    toggle.Set = std::move(set);
    field.Control = std::move(toggle);
    return field;
}

SettingsFieldDescriptor MakeLightDetailsRow(const char* label, const char* tooltip, bool defaultValue,
                                            LightDetailsGetter get, LightDetailsSetter set)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.Tooltip = tooltip;
    field.SearchKeywords = "gizmo light details range cone direction selection";
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = defaultValue;
    toggle.Get = [get]() { return (SceneViewSettings::Get().*get)(); };
    // Fired with the stored value when the page opens; SceneViewSettings saves
    // on every set, so an unchanged value returns early.
    toggle.Set = [get, set](bool value)
    {
        if ((SceneViewSettings::Get().*get)() == value)
            return;
        (SceneViewSettings::Get().*set)(value);
    };
    field.Control = std::move(toggle);
    return field;
}

SettingsFieldDescriptor MakeSizeSliderRow(const GizmoSliderRow& row, const TransformToolAccessor& transformTool,
                                          const std::shared_ptr<bool>& constantScreenSize)
{
    SettingsFieldDescriptor field;
    field.Label = row.Label;
    field.SearchKeywords = std::string("gizmo size ") + (row.SectionHeader ? row.SectionHeader : "") + " " +
                           row.Label + (row.ShownWithConstantScreenSize ? " constant screen size" : "");
    if (row.SectionHeader)
        field.SectionHeader = row.SectionHeader;
    field.PrefKey = row.PrefKey;
    field.IsVisible = [constantScreenSize, shownWhen = row.ShownWithConstantScreenSize]()
    { return *constantScreenSize == shownWhen; };

    // The tool only holds the value in memory; the panel persists PrefKey on
    // commit, so the drag can apply live.
    auto apply = [transformTool, setter = row.Apply](float value)
    {
        if (TransformTool* tool = transformTool())
            (tool->*setter)(value);
    };
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = row.DefaultValue;
    slider.MinValue = row.MinValue;
    slider.MaxValue = row.MaxValue;
    slider.Step = row.Step;
    slider.Preview = apply;
    slider.Set = apply;
    field.Control = std::move(slider);
    return field;
}
} // namespace

void RegisterGizmoSettingsCategory(std::function<TransformTool*()> transformTool)
{
    SettingsCategoryDescriptor gizmo;
    gizmo.CategoryId = "gizmo";
    gizmo.Title = "Gizmo";
    gizmo.Group = SettingsCategoryGroup::UserSettings;
    gizmo.TreeRowClass = "gizmo-row";
    gizmo.SearchKeywords = "gizmo transform handles translate rotate scale";

    // Which size set applies; the Constant Screen Size row writes it, including
    // once with the stored value each time the page is built.
    auto constantScreenSize = std::make_shared<bool>(kDefaultConstantScreenSize);

    gizmo.Fields.push_back(MakeToolToggleRow(
        "Constant Screen Size", kPrefKeyConstantScreenSize, kDefaultConstantScreenSize,
        [transformTool, constantScreenSize](bool enabled)
        {
            *constantScreenSize = enabled;
            if (TransformTool* tool = transformTool())
                tool->SetConstantScreenSize(enabled);
        }));
    // Front-face culled rings, a screen-space rotation ring and a smooth
    // edge-on drag; off keeps the plain rotate gizmo.
    gizmo.Fields.push_back(MakeToolToggleRow("Enhanced Rotate Gizmo", kPrefKeyRotateEnhanced,
                                             kDefaultRotateEnhanced,
                                             [transformTool](bool enabled)
                                             {
                                                 if (TransformTool* tool = transformTool())
                                                     tool->SetRotateGizmoEnhanced(enabled);
                                             }));

    // Light range, cone and direction details, separate from the icons that
    // always show.
    gizmo.Fields.push_back(MakeLightDetailsRow(
        "Directional Light Details", "Directional Gizmo Details Only On Selection",
        SceneViewSettings::kDefaultDirectionalLightGizmoDetailsOnlyOnSelection,
        &SceneViewSettings::GetDirectionalLightGizmoDetailsOnlyOnSelection,
        &SceneViewSettings::SetDirectionalLightGizmoDetailsOnlyOnSelection));
    gizmo.Fields.push_back(MakeLightDetailsRow(
        "Point Light Details", "Point Gizmo Details Only On Selection",
        SceneViewSettings::kDefaultPointLightGizmoDetailsOnlyOnSelection,
        &SceneViewSettings::GetPointLightGizmoDetailsOnlyOnSelection,
        &SceneViewSettings::SetPointLightGizmoDetailsOnlyOnSelection));
    gizmo.Fields.push_back(MakeLightDetailsRow(
        "Spot Light Details", "Spot Gizmo Details Only On Selection",
        SceneViewSettings::kDefaultSpotLightGizmoDetailsOnlyOnSelection,
        &SceneViewSettings::GetSpotLightGizmoDetailsOnlyOnSelection,
        &SceneViewSettings::SetSpotLightGizmoDetailsOnlyOnSelection));

    for (const GizmoSliderRow& row : kSliderRows)
        gizmo.Fields.push_back(MakeSizeSliderRow(row, transformTool, constantScreenSize));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(gizmo));
}

void ApplySavedGizmoSettings(TransformTool& tool)
{
    auto prefs = OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);

    bool constantScreenSize = kDefaultConstantScreenSize;
    prefs.TryGetBool(kPrefKeyConstantScreenSize, constantScreenSize);
    tool.SetConstantScreenSize(constantScreenSize);

    bool rotateEnhanced = kDefaultRotateEnhanced;
    prefs.TryGetBool(kPrefKeyRotateEnhanced, rotateEnhanced);
    tool.SetRotateGizmoEnhanced(rotateEnhanced);

    for (const GizmoSliderRow& row : kSliderRows)
    {
        double stored = row.DefaultValue;
        prefs.TryGetDouble(row.PrefKey, stored);
        (tool.*row.Apply)(std::clamp(static_cast<float>(stored), row.MinValue, row.MaxValue));
    }
}

} // namespace GameEngine::Editor
