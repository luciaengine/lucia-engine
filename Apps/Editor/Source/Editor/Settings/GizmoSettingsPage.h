#pragma once

#include <functional>

namespace GameEngine::Editor
{
namespace SceneTools
{
class TransformTool;
}

// Registers the "Gizmo" user-settings page with Editor::EditorSettingsRegistry.
// Main-thread only. transformTool returns the tool the rows drive, or nullptr
// while there is none; it must stay callable for as long as the Settings panel
// can build pages.
void RegisterGizmoSettingsCategory(std::function<SceneTools::TransformTool*()> transformTool);

// Pushes the saved gizmo preferences into a tool, so they apply from startup
// rather than from the first time the Gizmo page is opened.
void ApplySavedGizmoSettings(SceneTools::TransformTool& tool);

} // namespace GameEngine::Editor
