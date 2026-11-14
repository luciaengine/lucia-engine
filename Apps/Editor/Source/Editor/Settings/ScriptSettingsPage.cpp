#include "Editor/Settings/ScriptSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/ScriptEditorSettings.h"
#include "UI/SyntaxHighlightSettings.h"

#include <cstdint>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr float kLineHeightStep = 0.05f;

using ScriptToggleGetter = bool (ScriptEditorSettings::*)() const;
using ScriptToggleSetter = void (ScriptEditorSettings::*)(bool);
using SyntaxColor = uint32_t SyntaxHighlightSettings::*;

SettingsFieldDescriptor MakeScriptToggleRow(const char* label, const char* tooltip, const char* searchKeywords,
                                            bool defaultValue, ScriptToggleGetter get, ScriptToggleSetter set)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.Tooltip = tooltip;
    field.SearchKeywords = searchKeywords;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = defaultValue;
    toggle.Get = [get]() { return (ScriptEditorSettings::Get().*get)(); };
    // Fired with the stored value when the page opens; ScriptEditorSettings
    // saves on every set, so an unchanged value returns early.
    toggle.Set = [get, set](bool value)
    {
        if ((ScriptEditorSettings::Get().*get)() == value)
            return;
        (ScriptEditorSettings::Get().*set)(value);
    };
    field.Control = std::move(toggle);
    return field;
}

// The script editor reads SyntaxHighlightSettings as it draws, so writing the
// member is the live preview; Save is the commit.
SettingsFieldDescriptor MakeSyntaxColorRow(const char* label, uint32_t defaultArgb, SyntaxColor member)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = "syntax highlighting color colour script editor keyword string comment";
    SettingsFieldDescriptor::ColorField color;
    color.DefaultArgb = defaultArgb;
    color.ShowHexField = true;
    color.Get = [member]() { return SyntaxHighlightSettings::Get().*member; };
    color.Preview = [member](uint32_t argb) { SyntaxHighlightSettings::Get().*member = argb; };
    color.Set = [member](uint32_t argb)
    {
        SyntaxHighlightSettings::Get().*member = argb;
        SyntaxHighlightSettings::Get().Save();
    };
    field.Control = std::move(color);
    return field;
}
} // namespace

void RegisterScriptSettingsCategory()
{
    SettingsCategoryDescriptor script;
    script.CategoryId = "script";
    script.Title = "Script";
    script.Group = SettingsCategoryGroup::UserSettings;
    script.TreeRowClass = "script-row";
    script.SearchKeywords = "script editor code ide";

    SettingsFieldDescriptor openInInspector = MakeScriptToggleRow(
        "Open in Script Inspector", "Double-click opens in Script Inspector", "script inspector open double click ide",
        ScriptEditorSettings::kDefaultOpenInScriptInspector, &ScriptEditorSettings::GetOpenInScriptInspector,
        &ScriptEditorSettings::SetOpenInScriptInspector);
    openInInspector.SectionHeader = "Script Editor";
    openInInspector.InfoCard = "When disabled, double-clicking a script or log error opens in your default IDE.";
    script.Fields.push_back(std::move(openInInspector));

    SettingsFieldDescriptor glslInGraph = MakeScriptToggleRow(
        ".glsl Opens Material Graph", "Open shader graph .glsl in Material Graph",
        "glsl shader graph material graph open double click",
        ScriptEditorSettings::kDefaultOpenShaderGraphGlslInMaterialGraph,
        &ScriptEditorSettings::GetOpenShaderGraphGlslInMaterialGraph,
        &ScriptEditorSettings::SetOpenShaderGraphGlslInMaterialGraph);
    glslInGraph.InfoCard = "When disabled, material graph .glsl files (with @sg-graph tags) open in the Script "
                           "Editor instead. Other .glsl files always use the Script Editor.";
    script.Fields.push_back(std::move(glslInGraph));

    {
        // The script editor style is built from the saved line height, so the
        // row has no Preview: the release saves, then restyles.
        SettingsFieldDescriptor lineHeight;
        lineHeight.Label = "Line Height";
        lineHeight.SearchKeywords = "script editor line height spacing";
        SettingsFieldDescriptor::SliderField slider;
        slider.DefaultValue = ScriptEditorSettings::kDefaultLineHeight;
        slider.MinValue = ScriptEditorSettings::kMinLineHeight;
        slider.MaxValue = ScriptEditorSettings::kMaxLineHeight;
        slider.Step = kLineHeightStep;
        slider.Get = []() { return ScriptEditorSettings::Get().GetLineHeight(); };
        slider.Set = [](float value) { ScriptEditorSettings::Get().SetLineHeight(value); };
        lineHeight.Control = std::move(slider);
        script.Fields.push_back(std::move(lineHeight));
    }

    SettingsFieldDescriptor defaultText = MakeSyntaxColorRow(
        "Default Text", SyntaxHighlightSettings::kDefaultTextColor, &SyntaxHighlightSettings::DefaultColor);
    defaultText.SectionHeader = "Syntax Highlighting";
    script.Fields.push_back(std::move(defaultText));
    script.Fields.push_back(MakeSyntaxColorRow("Keywords", SyntaxHighlightSettings::kDefaultKeywordColor,
                                               &SyntaxHighlightSettings::KeywordColor));
    script.Fields.push_back(MakeSyntaxColorRow("Strings", SyntaxHighlightSettings::kDefaultStringColor,
                                               &SyntaxHighlightSettings::StringColor));
    script.Fields.push_back(MakeSyntaxColorRow("Comments", SyntaxHighlightSettings::kDefaultCommentColor,
                                               &SyntaxHighlightSettings::CommentColor));
    script.Fields.push_back(MakeSyntaxColorRow("Numbers", SyntaxHighlightSettings::kDefaultNumberColor,
                                               &SyntaxHighlightSettings::NumberColor));
    script.Fields.push_back(MakeSyntaxColorRow("Types", SyntaxHighlightSettings::kDefaultTypeColor,
                                               &SyntaxHighlightSettings::TypeColor));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(script));
}

} // namespace GameEngine::Editor
