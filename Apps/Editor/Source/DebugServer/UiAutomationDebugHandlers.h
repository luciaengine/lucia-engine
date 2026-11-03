#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// UI automation handlers: dump the UI and panel trees, inject pointer, click, drag, key and
// text input, and drive editor context menus without opening a native popup.
void RegisterUiAutomationDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
