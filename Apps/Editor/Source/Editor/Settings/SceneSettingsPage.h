#pragma once

namespace GameEngine::Editor
{

// Registers the "Scene" project-settings page (auto-save backup) with
// Editor::EditorSettingsRegistry. Main-thread only, once at editor startup;
// the SettingsPanel builds the rows from the descriptor.
void RegisterSceneSettingsCategory();

} // namespace GameEngine::Editor
