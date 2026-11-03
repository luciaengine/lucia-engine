#pragma once

#include <functional>

namespace GameEngine::Editor
{

// Registers the "Grid & Snapping" user-settings page with
// Editor::EditorSettingsRegistry. Main-thread only. applySnapSize pushes a
// committed snap size to the scene view that snaps with it; it must outlive
// the Settings panel's pages.
void RegisterGridSettingsCategory(std::function<void(float)> applySnapSize);

} // namespace GameEngine::Editor
