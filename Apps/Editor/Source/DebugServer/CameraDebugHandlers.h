#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Scene View camera handlers: read and set the editor camera pose, frame an entity or
// point, tween the camera, and switch the view between 2D and 3D.
void RegisterCameraDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
