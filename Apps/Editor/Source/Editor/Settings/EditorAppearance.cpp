#include "Editor/Settings/EditorAppearance.h"

#include "Editor/Settings/AccentColorSettings.h"
#include "Editor/Settings/CompactComponentHeadersSettings.h"
#include "Editor/Settings/GraySlidersSettings.h"
#include "Editor/Settings/InfoCardAppearanceSettings.h"
#include "Editor/Settings/InspectorRowLayoutSettings.h"
#include "Editor/Settings/PopupShadowSettings.h"
#include "Editor/Settings/TabIconsSettings.h"
#include "Editor/Settings/TextRenderingSettings.h"
#include "Editor/Settings/ToggleAppearanceSettings.h"
#include "Editor/Settings/ValueBoxHeightSettings.h"

namespace GameEngine::Editor
{

void ApplyEditorAppearance(UIManager* ui)
{
    if (!ui)
        return;

    // The owners that add stylesheets (accent, compact headers, row layout)
    // append them after the theme, whose rules and variables they override.
    AccentColorSettings::Get().ApplyTo(ui);
    CompactComponentHeadersSettings::Get().ApplyTo(ui);
    GraySlidersSettings::Get().ApplyTo(ui);
    ToggleAppearanceSettings::Get().ApplyTo(ui);
    TabIconsSettings::Get().ApplyTo(ui);
    PopupShadowSettings::Get().ApplyTo(ui);
    InfoCardAppearanceSettings::Get().ApplyTo(ui);
    InspectorRowLayoutSettings::Get().ApplyTo(ui);
    ValueBoxHeightSettings::Get().ApplyTo(ui);
    TextRenderingSettings::Get().ApplyTo(ui);
}

} // namespace GameEngine::Editor
