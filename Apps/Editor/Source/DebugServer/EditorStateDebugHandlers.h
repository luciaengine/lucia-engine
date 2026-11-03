#pragma once

namespace Logger
{
class RingBufferSink;
}

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Editor state and window handlers: the editor-state summary, the log ring, window
// placement, programmatic scroll, panel undocking, named editor commands, view focus
// and shutdown. The RingBufferSink backs get_log; it may be null, and get_log then
// reports that the ring is unavailable.
void RegisterEditorStateDebugHandlers(EditorDebugServer& server,
                                      EditorApplication& app,
                                      Logger::RingBufferSink* ringBufferSink);

} // namespace GameEngine
