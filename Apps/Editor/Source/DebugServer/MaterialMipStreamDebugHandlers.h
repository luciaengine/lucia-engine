#pragma once

namespace GameEngine
{

class EditorDebugServer;

// Readback of the material mip streams: the level and the screen coverage of each stream, in the
// order in which the planner gives detail.
void RegisterMaterialMipStreamDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
