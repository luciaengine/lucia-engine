#pragma once

namespace GameEngine
{

class EditorDebugServer;

// Mesh LOD handlers: dump every live MeshGPURegistry entry with its LOD chain, and drive
// the GPU LOD selection knobs (mode, bias, budgets, forced level, crossfade).
void RegisterMeshLodDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
