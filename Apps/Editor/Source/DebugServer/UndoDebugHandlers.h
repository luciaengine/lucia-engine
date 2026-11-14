#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Undo handlers: drive the editor undo stack like Ctrl+Z / Ctrl+Y and dump both stacks.
void RegisterUndoDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
