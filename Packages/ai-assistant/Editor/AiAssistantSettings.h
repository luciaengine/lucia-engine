#pragma once

#include "AssistantMode.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace Editor
{
struct SettingsCategoryDescriptor;
}

/// The AI Assistant's user settings: which connection the panel talks to, each
/// connection's model, the two CLI executables, the Node.js executable that runs the
/// editor's MCP server and the mode new conversations start in, kept in the editor
/// preferences under `aiAssistant.*`; and each local session's last session and its
/// mode per project, kept in the per-user settings of that project. There is no key setting: the Claude (API) connection reads
/// its key from the environment when a turn starts (ClaudeApiProvider).
class AiAssistantSettings
{
public:
    /// The settings page id and the prefix of every preference key.
    static constexpr std::string_view kCategoryId = "aiAssistant";

    /// A connection the user can choose.
    struct ProviderChoice
    {
        /// The provider's IAgentProvider::Id().
        std::string_view Id;
        /// What the settings page and the panel show.
        std::string_view Label;
    };
    /// Every connection, in the order the settings page and the panel list them.
    static std::span<const ProviderChoice> ProviderChoices();

    /// The connection id the panel uses (an IAgentProvider::Id()); the Claude
    /// session when none was chosen or the stored one is unknown.
    static std::string Provider();
    /// Stores the connection id; an unknown id is ignored.
    static void SetProvider(const std::string& providerId);
    /// Counts the SetProvider() calls that changed the stored connection in this
    /// process, so an open panel can follow a choice made on the settings page.
    static uint64_t ProviderGeneration();

    /// The model a turn of `providerId` asks for; empty selects the connection's
    /// own default.
    static std::string Model(std::string_view providerId);

    /// The `claude` executable: a program name searched for on PATH or a full path.
    static std::string ClaudeExecutable();
    /// The `codex` executable; empty until the user sets it, because Codex is not
    /// normally on PATH.
    static std::string CodexExecutable();

    /// The Node.js executable that runs the editor's MCP server for a conversation
    /// that acts in the editor: a program name searched for on PATH or a full path.
    static std::string NodeExecutable();
    static constexpr float kDefaultTextSizePx = 14.0f;
    static constexpr float kMaxTextSizePx = 24.0f;
    static float TextSize();
    static void SetTextSize(float px);

    /// The mode a new conversation starts in: the one last chosen, Auto until the
    /// user first chooses.
    static AssistantMode LastMode();
    /// Stores the mode last chosen.
    static void SetLastMode(AssistantMode mode);

    /// The mode the session `sessionId` of `providerId` last ran in, in the project at
    /// `projectRoot`; nullopt when that session is not the connection's last session
    /// there (a session this editor never ran, or an older one).
    static std::optional<AssistantMode> SessionMode(std::string_view providerId,
                                                    const std::filesystem::path& projectRoot,
                                                    std::string_view sessionId);
    /// Stores `mode` as the mode of the session `sessionId` of `providerId`, beside its
    /// last session (`aiAssistant.sessionMode.<providerId>`). Does nothing when
    /// `projectRoot` or `sessionId` is empty.
    static void SetSessionMode(std::string_view providerId, const std::filesystem::path& projectRoot,
                               std::string_view sessionId, AssistantMode mode);

    /// The session `providerId` last ran in the project at `projectRoot`, kept in the
    /// editor's per-user settings for that project (`aiAssistant.session.<providerId>`)
    /// so a later editor run can resume it; empty when none is stored or
    /// `projectRoot` is empty.
    static std::string LastSession(std::string_view providerId, const std::filesystem::path& projectRoot);
    /// Stores the session `providerId` runs in the project at `projectRoot`; an empty
    /// `sessionId` removes it. Does nothing when `projectRoot` is empty. Reads and
    /// writes the settings file: call it off the UI thread when it may run often.
    static void SetLastSession(std::string_view providerId, const std::filesystem::path& projectRoot,
                               const std::string& sessionId);

    /// The settings page: Provider, the three Model rows, the mode new conversations
    /// start in, the two executables, the Node executable and the Login status row.
    static Editor::SettingsCategoryDescriptor BuildCategory();
    /// Registers BuildCategory() with the editor's settings registry.
    static void Register();
};
} // namespace GameEngine
