#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Render-graph introspection handlers: overview, passes, resources, pass detail,
// dependencies and validation of the main window's last compiled graph.
void RegisterRenderGraphDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
