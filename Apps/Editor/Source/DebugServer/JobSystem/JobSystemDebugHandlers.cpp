#include "DebugServer/JobSystem/JobSystemDebugHandlers.h"

#include "Core/Engine.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/JobSystem/JobSystemReport.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <chrono>
#include <cstdint>

namespace GameEngine
{

void RegisterJobSystemDebugHandlers(EditorDebugServer& server)
{
    server.RegisterHandler("get_job_system", [](const EditorDebugServer::RequestContext&) -> nlohmann::json
    {
        const JobSystem::JobSystemStatistics stats = EngineCore::GetInstance().GetJobSystem().GetStatistics();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return Editor::BuildJobSystemReport(
            stats, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
    });
}

} // namespace GameEngine
