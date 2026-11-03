#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Model spawning handlers: instantiate a ModelAsset into the scene (deferred, or
// synchronously for the humanoid test pair) with an optional animation clip, export a
// spawned model's materials, and dump the importer provenance table those runs write.
void RegisterModelSpawnDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
