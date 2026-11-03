#include "Editor/Settings/CompactComponentHeadersSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"

#include <memory>
#include <string>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kPrefKey = "ui.inspectorCompactComponentHeaders";

// Both heading levels, from one setting. A component header and the section headers inside it
// are read as one ladder, so a section bar left at the default height while the component bar
// above it compacts reads as a different kind of thing rather than a nested one.
constexpr const char* kCompactCss =
    ".inspector-section-header-row { height: 24px; min-height: 24px; max-height: 24px; }"
    ".rp-foldout.inspector-component-section > .foldout-header"
    " { height: 24px; min-height: 24px; max-height: 24px; }";
constexpr const char* kRegularCss =
    ".inspector-section-header-row { height: 28px; min-height: 28px; max-height: 28px; }"
    ".rp-foldout.inspector-component-section > .foldout-header"
    " { height: 28px; min-height: 28px; max-height: 28px; }";

} // namespace

CompactComponentHeadersSettings& CompactComponentHeadersSettings::Get()
{
    static CompactComponentHeadersSettings instance;
    return instance;
}

CompactComponentHeadersSettings::CompactComponentHeadersSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.TryGetBool(kPrefKey, m_Enabled);
}

void CompactComponentHeadersSettings::SetEnabled(bool enabled)
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.SetBool(kPrefKey, enabled);
    prefs.Save(&error);

    if (m_Enabled == enabled)
        return;
    m_Enabled = enabled;
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyToWindow(ui); });
}

void CompactComponentHeadersSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyToWindow(*ui);
}

void CompactComponentHeadersSettings::ApplyToWindow(UIManager& ui)
{
    auto sheet = std::make_shared<Stylesheet>();
    if (UIParsing::CSSParser::ParseStylesFromString(m_Enabled ? kCompactCss : kRegularCss, *sheet))
        m_Windows.SetOwnedStylesheet(ui, std::move(sheet));
}

} // namespace GameEngine::Editor
