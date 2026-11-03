#include "Editor/Settings/InspectorRowLayoutSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"

#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine::Editor
{

namespace
{

std::string InspectorRowCss(float indentPx, float gapPx, float labelWidthPercent)
{
    char css[4096];
    std::snprintf(css, sizeof(css),
        ".inspector-panel { --inspector-row-gap: %.0fpx; }"
        ".inspector-section:not(.inspector-section-transform) .inspector-section-body .inspector-row {"
        " padding-left: %.0fpx; }"
        ".inspector-section:not(.inspector-section-transform) .inspector-section-body .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".inspector-section:not(.inspector-section-transform) .inspector-section-body .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".inspector-asset-content > .inspector-row {"
        " padding-left: %.0fpx; }"
        ".inspector-asset-content > .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".inspector-asset-content > .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".material-inspector .inspector-row {"
        " padding-left: %.0fpx; }"
        ".material-properties-foldout .material-inspector .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".material-properties-foldout .material-inspector .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".rp-foldout .settings-row-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; }"
        ".rp-foldout .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".rp-foldout .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }",
        gapPx, indentPx,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        indentPx,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        indentPx,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent);
    return css;
}

// The Settings page row spacing follows the inspector row gap.
std::string SettingsRowGapCss(float gapPx)
{
    const float rowPaddingY = gapPx * 0.25f;
    const float rowMarginBottom = gapPx * 0.5f;
    const float headerPaddingY = gapPx * 0.25f;

    char css[2048];
    std::snprintf(css, sizeof(css),
        ".settings-panel .settings-content-body .scroll-content { gap: %.2fpx !important; }"
        ".settings-panel .settings-content { gap: %.2fpx !important; }"
        ".settings-panel .settings-row { margin-bottom: %.2fpx !important; padding-top: %.2fpx !important; padding-bottom: %.2fpx !important; }"
        ".settings-panel .settings-section-header { margin-top: %.2fpx !important; margin-bottom: %.2fpx !important; padding-top: %.2fpx !important; padding-bottom: %.2fpx !important; }"
        ".settings-panel .settings-subsection-header { margin-top: %.2fpx !important; margin-bottom: %.2fpx !important; padding-top: %.2fpx !important; padding-bottom: %.2fpx !important; }"
        ".settings-panel .settings-build-scene-row { margin-top: %.2fpx !important; }"
        ".settings-panel .settings-build-drop-zone { margin-bottom: %.2fpx !important; }",
        rowMarginBottom,
        rowMarginBottom,
        rowMarginBottom,
        rowPaddingY,
        rowPaddingY,
        rowMarginBottom,
        rowMarginBottom,
        headerPaddingY,
        headerPaddingY,
        rowMarginBottom,
        rowMarginBottom,
        headerPaddingY,
        headerPaddingY,
        rowMarginBottom,
        rowMarginBottom);
    return css;
}

} // namespace

InspectorRowLayoutSettings& InspectorRowLayoutSettings::Get()
{
    static InspectorRowLayoutSettings instance;
    return instance;
}

InspectorRowLayoutSettings::InspectorRowLayoutSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    auto loadFloat = [&prefs](const char* key, float& out) {
        double stored = out;
        if (prefs.TryGetDouble(key, stored))
            out = static_cast<float>(stored);
    };
    loadFloat(kPrefKeyIndent, m_IndentPx);
    loadFloat(kPrefKeyGap, m_GapPx);
    loadFloat(kPrefKeyLabelWidth, m_LabelWidthPercent);
}

void InspectorRowLayoutSettings::SetLayout(float indentPx, float gapPx, float labelWidthPercent)
{
    m_IndentPx = indentPx;
    m_GapPx = gapPx;
    m_LabelWidthPercent = labelWidthPercent;
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyToWindow(ui); });
}

void InspectorRowLayoutSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyToWindow(*ui);
}

// Inspector rows first, then the Settings page rows, in one sheet.
void InspectorRowLayoutSettings::ApplyToWindow(UIManager& ui)
{
    const std::string text =
        InspectorRowCss(m_IndentPx, m_GapPx, m_LabelWidthPercent) + SettingsRowGapCss(m_GapPx);

    auto sheet = std::make_shared<Stylesheet>();
    if (UIParsing::CSSParser::ParseStylesFromString(text.c_str(), *sheet))
        m_Windows.SetOwnedStylesheet(ui, std::move(sheet));
    else
        m_Windows.SetOwnedStylesheet(ui, nullptr);
}

} // namespace GameEngine::Editor
