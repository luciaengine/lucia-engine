#pragma once

namespace GameEngine
{

class EditorDebugServer;

// Crash handler: kill the editor through a chosen crash path so automation can check
// the crash-evidence pipeline end to end.
void RegisterCrashDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
