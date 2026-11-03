#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Movie recording handler: start, stop and report the Game View's movie capture, and
// hand a finished debug recording to the movie recorder.
void RegisterMovieRecordDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
