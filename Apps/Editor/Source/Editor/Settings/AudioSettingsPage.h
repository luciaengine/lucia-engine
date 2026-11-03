#pragma once

namespace GameEngine::Editor
{

// Registers the "Audio Settings" project-settings page with
// Editor::EditorSettingsRegistry. Main-thread only, once at editor startup;
// the SettingsPanel builds the rows from the descriptor.
void RegisterAudioSettingsCategory();

} // namespace GameEngine::Editor
