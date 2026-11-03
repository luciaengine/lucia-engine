#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Render statistics handlers: validation-layer telemetry, the get_render_stats frame
// report (render services, residency, culling, shadows, frame and UI phase timings, GPU
// sync), UI clip-slot usage and the UI update-profile history.
void RegisterRenderStatsDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
