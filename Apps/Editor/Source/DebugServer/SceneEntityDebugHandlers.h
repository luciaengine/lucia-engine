#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// Scene and entity handlers: hierarchy and component dumps, ECS queries, and
// create/set_component/enable/delete/select on entities. set_component writes reflected
// components by field name, a few components through named branches, and schema-only
// components through their scene schema.
void RegisterSceneEntityDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
