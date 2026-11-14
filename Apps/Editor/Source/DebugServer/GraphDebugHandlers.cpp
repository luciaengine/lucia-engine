#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterGraphDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("graph_set_viewport", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        float panX = 0.f;
        float panY = 0.f;
        float zoom = 1.f;
        canvas->GetPanZoom(panX, panY, zoom);
        panX = ctx.params.value("panX", panX);
        panY = ctx.params.value("panY", panY);
        zoom = ctx.params.value("zoom", zoom);
        canvas->SetPanZoom(panX, panY, zoom);
        canvas->GetPanZoom(panX, panY, zoom);
        return json{{"ok", true}, {"panX", panX}, {"panY", panY}, {"zoom", zoom}}; });

server.RegisterHandler("graph_get_nodes", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        const Graph::Model* model = nodeGraphPanel->ActiveGraphModel();
        if (!model)
            return Editor::RefuseRequest("No graph model");
        json nodes = json::array();
        for (const Graph::Node& node : model->Nodes)
        {
            nodes.push_back(json{{"id", node.Id},
                                 {"typeId", node.TypeId},
                                 {"x", node.PositionX},
                                 {"y", node.PositionY}});
        }
        return json{{"ok", true}, {"nodes", nodes}}; });

server.RegisterHandler("graph_delete_node", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string nodeId = ctx.params.value("nodeId", std::string{});
        if (nodeId.empty())
            return Editor::RefuseRequest("Provide nodeId");
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        const Graph::Model* model = nodeGraphPanel->ActiveGraphModel();
        if (!model)
            return Editor::RefuseRequest("No graph model");
        const auto& nodes = model->Nodes;
        const bool exists = std::any_of(nodes.begin(), nodes.end(),
                                        [&nodeId](const Graph::Node& n) { return n.Id == nodeId; });
        if (!exists)
            return Editor::RefuseRequest("No node with id: " + nodeId);
        /* Same path as the Delete key: selection + DeleteSelectedNodes keeps
           undo, link cleanup and widget recycling on the one implementation. */
        canvas->SetSelectedNodeId(nodeId);
        canvas->DeleteSelectedNodes();
        return json{{"ok", true}, {"deleted", nodeId}}; });
}
}
