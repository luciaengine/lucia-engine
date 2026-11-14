#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterCrashDebugHandlers(EditorDebugServer& server)
{
server.RegisterHandler("debug_crash", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string kind = ctx.params.value("kind", "av");
        if (kind == "av")
        {
            volatile int* ptr = nullptr;
            *ptr = 42; // intentional AV → UnhandledSEHFilter
        }
        else if (kind == "terminate")
        {
            std::terminate(); // → TerminateHandler
        }
        else if (kind == "abort")
        {
            std::abort(); // → SIGABRT → SignalHandler
        }
        return Editor::RefuseRequest("Unknown crash kind: " + kind + " (expected av|terminate|abort)"); });
}
}
