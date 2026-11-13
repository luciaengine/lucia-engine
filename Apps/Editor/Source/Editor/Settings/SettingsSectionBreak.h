#pragma once

#include "UI/UIElement.h"

namespace GameEngine::Editor
{

struct SettingsFieldDescriptor;

// The break above a settings row that starts a named group: the SectionHeader line, then the
// SectionDescription as an info card that the "Show Info Cards" switch hides. A part with no text
// is hidden. The layout is UI/controls/SettingsSectionBreak/SettingsSectionBreak.uxml.
class SettingsSectionBreak : public UIElement
{
public:
    explicit SettingsSectionBreak(const SettingsFieldDescriptor& field);
};

} // namespace GameEngine::Editor
