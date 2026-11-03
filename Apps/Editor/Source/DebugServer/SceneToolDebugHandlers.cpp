#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterSceneToolDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("set_scene_tool", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string toolName = ctx.params.value("tool", "");
        if (toolName.empty())
            return Editor::RefuseRequest("Provide 'tool' name: selection, transform, navgrid, or a tool strip entry's id (spline, terrainBrush, markups)");

        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* ctrl = app.m_Windows[0]->scene.get();

        using TK = SceneViewController::ToolKind;
        // The built-in kinds by name; any other name is a tool strip entry's id.
        if (toolName == "selection")           ctrl->SetActiveTool(TK::Selection);
        else if (toolName == "transform")      ctrl->SetActiveTool(TK::Transform);
        else if (toolName == "navgrid")        ctrl->SetActiveTool(TK::NavGridBrush);
        else if (!ctrl->SetActiveRegisteredTool(toolName))
            return Editor::RefuseRequest(
                Editor::RegisteredToolRefusal(Editor::SceneViewToolStripRegistry::Get(), toolName, &ctrl->GetWorld()));
        return json{{"ok", true}, {"tool", toolName}}; });

server.RegisterHandler("set_terrain_brush", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* brush = dynamic_cast<Editor::SceneTools::TerrainBrushTool*>(
            app.m_Windows[0]->scene->GetRegisteredTool("terrainBrush"));
        if (!brush)
            return Editor::RefuseRequest("No terrain brush tool");

        if (ctx.params.contains("radius"))
            brush->SetRadius(ctx.params["radius"].get<float>());
        if (ctx.params.contains("strength"))
            brush->SetStrength(ctx.params["strength"].get<float>());
        if (ctx.params.contains("mode"))
        {
            using Mode = GameEngine::Editor::SceneTools::TerrainBrushMode;
            const std::string m = ctx.params["mode"].get<std::string>();
            if (m == "raise")      brush->SetMode(Mode::Raise);
            else if (m == "lower") brush->SetMode(Mode::Lower);
            else if (m == "paint") brush->SetMode(Mode::Paint);
            else return Editor::RefuseRequest("mode must be raise|lower|paint");
        }
        return json{{"ok", true}, {"radius", brush->GetRadius()}, {"strength", brush->GetStrength()}}; });

server.RegisterHandler("set_gizmos_visibility", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");

        // Name the offending group: an uncaught type_error reports only
        // "type must be boolean", which does not say which one was wrong.
        for (const char* key : {"all", "light", "transform", "markups"})
        {
            if (ctx.params.contains(key) && !ctx.params[key].is_boolean())
                return Editor::RefuseRequest(std::string("'") + key + "' must be a boolean");
        }

        Editor::SceneViewRenderCoordinator::ForEachController(
            app.m_Windows[0].get(),
            [&ctx](SceneViewController* controller)
            {
                if (ctx.params.contains("all"))
                    controller->SetGizmosVisible(ctx.params["all"].get<bool>());
                if (ctx.params.contains("light"))
                    controller->SetLightGizmosVisible(ctx.params["light"].get<bool>());
                if (ctx.params.contains("transform"))
                    controller->SetTransformGizmosVisible(ctx.params["transform"].get<bool>());
                if (ctx.params.contains("markups"))
                    controller->SetMarkupGizmosVisible(ctx.params["markups"].get<bool>());
            });

        const auto* ctrl = app.m_Windows[0]->scene.get();
        return json{{"ok", true},
                    {"all", ctrl->AreGizmosVisible()},
                    {"light", ctrl->AreLightGizmosVisible()},
                    {"transform", ctrl->AreTransformGizmosVisible()},
                    {"markups", ctrl->AreMarkupGizmosVisible()}}; });

server.RegisterHandler("get_scene_tool", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* ctrl = app.m_Windows[0]->scene.get();

        using TK = SceneViewController::ToolKind;
        std::string name = "unknown";
        switch (ctrl->GetActiveToolKind())
        {
            case TK::Selection:     name = "selection"; break;
            case TK::Transform:     name = "transform"; break;
            case TK::NavGridBrush:  name = "navgrid"; break;
            case TK::Registered:    name = std::string(ctrl->GetActiveRegisteredToolId()); break;
        }
        return json{{"tool", name}}; });
}
}
