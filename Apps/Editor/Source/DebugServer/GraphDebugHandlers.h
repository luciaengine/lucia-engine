#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Graph panel handlers: set the canvas pan/zoom, spawn spike nodes, list the active
// graph's nodes and delete a node, on the live graph panel.
void RegisterGraphDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
