#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Scene view tool handlers: read and switch the active scene tool, drive the terrain
// brush settings, and show or hide the scene view's gizmo overlays.
void RegisterSceneToolDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
