#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Asset handlers: ProFlares import, opening an asset in its editor panel, Assets panel
// and Asset View selection and preview state, the registered asset sources and their
// listings, and the skeleton store's per-runtime skinning state.
void RegisterAssetDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
