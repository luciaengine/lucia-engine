#pragma once

namespace GameEngine::Editor
{

// Registers the "Asset Import" project-settings page (SVG texture raster size,
// FBX import options, automatic LODs) with Editor::EditorSettingsRegistry.
// Main-thread only, once at editor startup; the SettingsPanel builds the rows
// from the descriptor.
void RegisterAssetImportSettingsCategory();

} // namespace GameEngine::Editor
