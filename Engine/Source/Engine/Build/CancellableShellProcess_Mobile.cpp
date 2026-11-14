#include "Engine/Build/CancellableShellProcess.h"

namespace GameEngine
{
namespace
{
ShellProcessResult Unavailable()
{
    ShellProcessResult result;
    result.stderrTail = "Build processes are unavailable inside the mobile Player; compile code in the Editor.";
    return result;
}
}
CancellableShellProcess::~CancellableShellProcess() = default;
ShellProcessResult RunProcessCaptured(const std::string&, const std::vector<std::string>&, std::chrono::milliseconds) { return Unavailable(); }
ShellProcessResult CancellableShellProcess::Run(const std::string&, std::function<bool()>, std::function<void(const std::string&)>) { return Unavailable(); }
ShellProcessResult CancellableShellProcess::Run(const ProcessLaunch&, std::function<bool()>, std::function<void(const std::string&)>) { return Unavailable(); }
void CancellableShellProcess::Kill() {}
void CancellableShellProcess::TerminateChildLocked() {}
}
