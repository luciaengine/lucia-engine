#include "HarnessSessionProvider.h"

#include "JsonFields.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
constexpr std::string_view kUnknownSessionText = "does not exist; omit --session-id";
} // namespace

ProviderCapabilities HarnessSessionProvider::Capabilities() const
{
    return {.Streams = true, .OwnsTranscript = true, .NeedsKey = false, .CanActWithTools = false};
}

CliSessionList HarnessSessionProvider::ListSessions(const CancelToken&) const
{
    return {};
}

std::vector<std::string> HarnessSessionProvider::TurnArguments(const AgentTurnRequest& request) const
{
    std::vector<std::string> arguments = {"--profile",       "headless", "--json", "--permission-mode",
                                          "read-only",       "--approval-policy", "never"};
    if (!request.SessionId.empty())
    {
        arguments.push_back("--session-id");
        arguments.push_back(request.SessionId);
    }
    arguments.push_back("-");
    return arguments;
}

void HarnessSessionProvider::ReadLine(std::string_view line, TurnState& turn) const
{
    const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
    if (!record.is_object())
        return;

    TurnResult& result = turn.Result;
    const std::string type = JsonFields::String(record, "type");
    if (type == "session")
    {
        if (const std::string sessionId = JsonFields::String(record, "sessionId"); !sessionId.empty())
            turn.Events.OnSessionId(sessionId);
    }
    else if (type == "text")
    {
        // Each committed message arrives whole, as a block of its own.
        turn.TextBlocks.BeginBlock();
        const std::string_view appended = turn.TextBlocks.Append(result.Text, JsonFields::String(record, "text"));
        if (!appended.empty())
            turn.Events.OnTextDelta(appended);
    }
    else if (type == "tool_call")
    {
        turn.Events.OnToolActivity({JsonFields::String(record, "tool"), record.dump()});
    }
    else if (type == "status" && JsonFields::String(record, "phase") == "step_end")
    {
        const nlohmann::json& usage = JsonFields::Object(record, "usage");
        result.InputTokens += JsonFields::Count(usage, "inputTokens");
        result.OutputTokens += JsonFields::Count(usage, "outputTokens");
        ++result.ModelRoundTrips;
    }
    else if (type == "final")
    {
        turn.Completed = true;
        // The final text is the whole answer; it fills the reply when no text record came.
        if (result.Text.empty())
        {
            const std::string_view appended = turn.TextBlocks.Append(result.Text, JsonFields::String(record, "text"));
            if (!appended.empty())
                turn.Events.OnTextDelta(appended);
        }
        const nlohmann::json& reason = JsonFields::Object(record, "reason");
        const std::string kind = JsonFields::String(reason, "kind");
        if (kind == "completed")
        {
            result.Outcome = TurnOutcome::Succeeded;
            return;
        }
        result.Outcome = TurnOutcome::Failed;
        const std::string message = JsonFields::String(JsonFields::Object(reason, "error"), "message");
        result.Error = "DeepSeek Harness ended the turn without completing it (" +
                       (message.empty() ? (kind.empty() ? std::string("no reason given") : kind) : message) + ").";
    }
    else if (type == "error")
    {
        const std::string message = JsonFields::String(record, "message");
        turn.Completed = true;
        result.Outcome = TurnOutcome::Failed;
        result.SessionNotFound = message.find(kUnknownSessionText) != std::string::npos;
        result.Error = "DeepSeek Harness reported an error: " + (message.empty() ? std::string("no detail given") : message);
    }
}

SetupStep HarnessSessionProvider::CheckProgram(const CancelToken& cancel, SetupSurface surface) const
{
    constexpr std::string_view kCommand = "dsh --version";
    const CommandOutput output = RunCommand({"--version"}, cancel);
    if (std::optional<SetupStep> failed = FailedProgramStep(output, kCommand, surface))
        return *std::move(failed);
    if (output.ExitCode != 0)
        return NotTheCliStep(output, surface);
    return {.Name = "DeepSeek Harness",
            .State = SetupStepState::Passed,
            .Found = "dsh " + output.FirstOutput + " at " + Executable() + ".",
            .Program = Executable(),
            .ExitCode = output.ExitCode,
            .FirstOutput = output.FirstOutput};
}

SetupStep HarnessSessionProvider::CheckLogin(const CancelToken&, SetupSurface) const
{
    return {.Name = "Model",
            .State = SetupStepState::Passed,
            .Found = "Uses the model DeepSeek Harness is set to use; change it in Harness.",
            .Program = Executable()};
}

std::string HarnessSessionProvider::ProgramToPick() const
{
    return "the dsh program inside Harness.app (Contents/Resources/runtime/cli/bin/dsh)";
}

std::string HarnessSessionProvider::InstallSource() const
{
    return "install DeepSeek Harness";
}

std::string HarnessSessionProvider::DesktopAppHint(bool) const
{
    return {};
}

bool HarnessSessionProvider::IsUnknownSessionError(std::string_view stderrTail) const
{
    return stderrTail.find(kUnknownSessionText) != std::string_view::npos;
}
} // namespace GameEngine
