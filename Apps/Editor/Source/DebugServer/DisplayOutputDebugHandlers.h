#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Display-output handlers: HDR output mode and state, MSAA sample count, anti-aliasing
// mode, TAA render scale and dynamic resolution.
void RegisterDisplayOutputDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
