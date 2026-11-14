#pragma once

#include <functional>

namespace GameEngine::Editor
{

// Registers the "Performance" user-settings page (graphics API, VSync, Scene
// View render scale) with Editor::EditorSettingsRegistry. Main-thread only.
// applyVsync switches VSync on the device that presents the editor; it is also
// called with the current state each time the page is built, and must stay
// callable for as long as the Settings panel can build pages.
void RegisterPerformanceSettingsCategory(std::function<void(bool)> applyVsync);

} // namespace GameEngine::Editor
