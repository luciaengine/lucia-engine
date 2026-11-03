#pragma once

namespace GameEngine::Editor
{

// Registers the "Animation" settings page (curve colours and widths, timeline
// and Animation window defaults) with Editor::EditorSettingsRegistry.
// Main-thread only, once at editor startup; the SettingsPanel builds the rows
// from the descriptor.
void RegisterAnimationSettingsCategory();

} // namespace GameEngine::Editor
