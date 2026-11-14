#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Capture handlers: take_screenshot (render-graph readback with a window-capture
// fallback), shadow diagnostics, capture of a named render-graph resource, RenderDoc
// triggering and status, and per-pass descriptor capture.
void RegisterCaptureDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
