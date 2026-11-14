#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

/**
 * Registers the tilemap authoring drivers: cell painting through the Scene View
 * tool, and tileset creation through the Tile Set Editor panel.
 *
 * Both go through the same entry points the interactive UI uses, so an
 * automated flow exercises the real undo boundary and the real save path rather
 * than a parallel implementation that could drift from them.
 */
void RegisterTileMapDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
