#pragma once

namespace GameEngine::Editor
{

// Registers the "Physics" project-settings page (gravity and simulation
// stepping) with Editor::EditorSettingsRegistry. Main-thread only, once at
// editor startup; the SettingsPanel builds the rows from the descriptor.
void RegisterPhysicsSettingsCategory();

} // namespace GameEngine::Editor
