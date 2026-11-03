#pragma once

namespace GameEngine::Editor
{

// Registers the "Input" project-settings page (Scene View mouse look) with
// Editor::EditorSettingsRegistry. Main-thread only, once at editor startup;
// the SettingsPanel builds the rows from the descriptor.
void RegisterInputSettingsCategory();

} // namespace GameEngine::Editor
