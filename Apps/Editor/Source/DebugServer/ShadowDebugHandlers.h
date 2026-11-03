#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Shadow debug handlers: cascade and factor debug views, cascade thumbnails, filter
// quality, PCSS, and the MSM moments resolution and blur mode.
void RegisterShadowDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
