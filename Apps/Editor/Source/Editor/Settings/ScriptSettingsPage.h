#pragma once

namespace GameEngine::Editor
{

// Registers the "Script" user-settings page (script editor behaviour, line
// height, syntax colours) with Editor::EditorSettingsRegistry. Main-thread
// only. A line-height change restyles the open script editors through
// ScriptEditorSettings::SetLineHeight.
void RegisterScriptSettingsCategory();

} // namespace GameEngine::Editor
