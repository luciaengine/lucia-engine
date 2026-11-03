#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Player-build handlers: start a build from the Build panel, report its status, and
// report the build settings resolved from project and editor preferences.
void RegisterBuildDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
