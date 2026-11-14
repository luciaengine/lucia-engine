#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Scene lifecycle handlers: open and save the scene document, answer the scene modals
// that block those, and drive the play-mode transitions that snapshot and restore the
// world.
void RegisterSceneLifecycleDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
