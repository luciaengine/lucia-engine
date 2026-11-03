#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterUndoDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("undo", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        const bool could = app.CanUndo();
        const char* nameBefore = app.GetUndoActionName();
        const std::string undoneName = nameBefore ? std::string(nameBefore) : std::string();

        app.Undo();

        const char* nu = app.GetUndoActionName();
        const char* nr = app.GetRedoActionName();
        return json{{"ok", true},
                    {"undone", could},
                    {"undoneName", undoneName},
                    {"canUndo", app.CanUndo()},
                    {"canRedo", app.CanRedo()},
                    {"nextUndo", nu ? std::string(nu) : std::string()},
                    {"nextRedo", nr ? std::string(nr) : std::string()}}; });

server.RegisterHandler("redo", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        const bool could = app.CanRedo();
        const char* nameBefore = app.GetRedoActionName();
        const std::string redoneName = nameBefore ? std::string(nameBefore) : std::string();

        app.Redo();

        const char* nu = app.GetUndoActionName();
        const char* nr = app.GetRedoActionName();
        return json{{"ok", true},
                    {"redone", could},
                    {"redoneName", redoneName},
                    {"canUndo", app.CanUndo()},
                    {"canRedo", app.CanRedo()},
                    {"nextUndo", nu ? std::string(nu) : std::string()},
                    {"nextRedo", nr ? std::string(nr) : std::string()}}; });

server.RegisterHandler("get_undo_stack", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* svc = app.m_UndoRedo.get();
        if (!svc)
            return Editor::RefuseRequest("No undo service");

        const std::size_t undoCount = svc->GetUndoCount();
        const std::size_t redoCount = svc->GetRedoCount();

        auto epochMs = [](std::chrono::system_clock::time_point tp) -> int64_t
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
        };

        // Undo: index 0 = oldest, last = next-to-undo (top of stack).
        json undo = json::array();
        for (std::size_t i = 0; i < undoCount; ++i)
        {
            const char* type = svc->GetUndoTypeNameAt(i);
            const char* label = svc->GetUndoNameAt(i);
            undo.push_back({{"index", static_cast<uint64_t>(i)},
                            {"type", type ? std::string(type) : std::string()},
                            {"label", label ? std::string(label) : std::string()},
                            {"timestampMs", epochMs(svc->GetUndoTimestampAt(i))},
                            {"isNextUndo", i + 1 == undoCount}});
        }

        // Redo: index 0 = next-to-redo (most recently undone).
        json redo = json::array();
        for (std::size_t i = 0; i < redoCount; ++i)
        {
            const char* type = svc->GetRedoTypeNameAt(i);
            const char* label = svc->GetRedoNameAt(i);
            redo.push_back({{"index", static_cast<uint64_t>(i)},
                            {"type", type ? std::string(type) : std::string()},
                            {"label", label ? std::string(label) : std::string()},
                            {"isNextRedo", i == 0}});
        }

        const char* nu = svc->PeekUndoName();
        const char* nr = svc->PeekRedoName();
        json result;
        result["undoCount"] = static_cast<uint64_t>(undoCount);
        result["redoCount"] = static_cast<uint64_t>(redoCount);
        result["cursor"] = static_cast<uint64_t>(undoCount);
        result["canUndo"] = svc->CanUndo();
        result["canRedo"] = svc->CanRedo();
        result["nextUndo"] = nu ? std::string(nu) : std::string();
        result["nextRedo"] = nr ? std::string(nr) : std::string();
        result["undo"] = std::move(undo);
        result["redo"] = std::move(redo);
        return result; });
}
}
