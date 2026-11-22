#pragma once

#include "CliSessionProvider.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
/// DeepSeek Harness, run as `dsh --profile headless --json` with the read-only permission preset
/// and no approval prompts, on the model Harness is set to use. `session` reports the session id;
/// each `text` record is one committed reply message; `status` records with phase `step_end`
/// carry each step's token usage; `final` ends the turn, and its reason's kind `completed` is
/// the only success. An `error` record fails the turn. Harness keeps its sessions in its own
/// store, which the panel does not list, and cannot attach the editor's tools.
class HarnessSessionProvider final : public CliSessionProvider
{
public:
    /// The provider's id (Id()).
    static constexpr std::string_view kId = "harness-session";
    /// The executable the settings default to: `dsh` on PATH, else where detection finds it.
    static constexpr std::string_view kDefaultExecutable = "dsh";

    using CliSessionProvider::CliSessionProvider;

    std::string_view Id() const override { return kId; }
    /// A local session that keeps its transcript, without the editor's tools.
    ProviderCapabilities Capabilities() const override;

    /// Empty: Harness's session store is its own format.
    CliSessionList ListSessions(const CancelToken& cancel) const override;

    /// `--profile headless --json --permission-mode read-only --approval-policy never`, then
    /// `--session-id <SessionId>` when it continues a session, and `-` (the task is read from
    /// stdin). Harness takes no model or system prompt option: it uses its own.
    std::vector<std::string> TurnArguments(const AgentTurnRequest& request) const override;

private:
    std::string_view DisplayName() const override { return "DeepSeek Harness"; }
    void ReadLine(std::string_view line, TurnState& turn) const override;
    /// `dsh --version` printed a version.
    SetupStep CheckProgram(const CancelToken& cancel, SetupSurface surface) const override;
    /// Harness has no login command: the step passes and says the model is Harness's own.
    SetupStep CheckLogin(const CancelToken& cancel, SetupSurface surface) const override;
    std::string ProgramToPick() const override;
    std::string InstallSource() const override;
    std::string DesktopAppHint(bool printedNothing) const override;
    /// `--session-id` with an id Harness does not know prints `session "<id>" does not exist`.
    bool IsUnknownSessionError(std::string_view stderrTail) const override;
};
} // namespace GameEngine
