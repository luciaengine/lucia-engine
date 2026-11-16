#pragma once

namespace GameEngine::Editor
{

/// Registers the Search Bars category on UI > Appearance. Its rows drive the EditorSearchBars state of
/// every registered panel search bar: Filled Search Bars, Search Bars at Top, Accent Search Focus Outline.
void RegisterSearchBarSettingsCategory();

} // namespace GameEngine::Editor
