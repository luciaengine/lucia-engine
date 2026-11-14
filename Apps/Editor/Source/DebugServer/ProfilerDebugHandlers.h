#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Profiler handlers: the CPU profiler tree and samples, per-pass render-graph GPU
// timings, and the DebugMetrics monitor snapshot and history.
void RegisterProfilerDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
