#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "AiAssistantSettings.h"

#include "AiAssistantLoginStatusRow.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace GameEngine
{
namespace { constexpr const char* kPrefTextSize = "aiAssistant.textSize"; }
namespace
{
constexpr const char* kPrefProvider = "aiAssistant.provider";
constexpr const char* kPrefClaudeSessionModel = "aiAssistant.claudeSessionModel";
constexpr const char* kPrefClaudeApiModel = "aiAssistant.claudeApiModel";
constexpr const char* kPrefCodexModel = "aiAssistant.codexModel";
constexpr const char* kPrefClaudeExecutable = "aiAssistant.claudeExecutable";
constexpr const char* kPrefCodexExecutable = "aiAssistant.codexExecutable";
constexpr const char* kPrefNodeExecutable = "aiAssistant.nodeExecutable";
constexpr const char* kPrefLastMode = "aiAssistant.lastMode";
constexpr const char* kDefaultNodeExecutable = "node";
constexpr AssistantMode kInitialMode = AssistantMode::Auto;
// The Claude session model row's value for "let Claude Code choose".
constexpr const char* kClaudeSessionDefaultModel = "default";

using Field = Editor::SettingsFieldDescriptor;

std::atomic<uint64_t> s_ProviderGeneration{0};

// Serializes the read-modify-write of a project's per-user settings: a turn's thread
// stores the session it ran while the panel stores the session's mode.
std::mutex s_ProjectSettingsMutex;

std::string ReadPreference(const char* key, std::string fallback)
{
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    prefs.TryGetString(key, fallback);
    return fallback;
}

void WritePreference(const char* key, const std::string& value)
{
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    std::string stored;
    if (prefs.TryGetString(key, stored) && stored == value)
        return;
    prefs.SetString(key, value);
    prefs.Save();
}

// The per-project key of a connection's last session.
std::string SessionKey(std::string_view providerId)
{
    return "aiAssistant.session." + std::string(providerId);
}

// The per-project key of the mode of a connection's last session, stored as
// "<mode id>:<session id>" so a mode never applies to another session.
std::string SessionModeKey(std::string_view providerId)
{
    return "aiAssistant.sessionMode." + std::string(providerId);
}

constexpr AiAssistantSettings::ProviderChoice kProviderChoices[] = {
    {ClaudeSessionProvider::kId, "Claude (local session)"},
    {CodexSessionProvider::kId, "Codex (local session)"},
    {ClaudeApiProvider::kId, "Claude (API)"},
};

bool IsKnownProvider(std::string_view id)
{
    for (const AiAssistantSettings::ProviderChoice& choice : kProviderChoices)
        if (choice.Id == id)
            return true;
    return false;
}

Field DropdownRow(std::string label, std::string tooltip, const char* key, std::string defaultValue,
                  std::vector<Field::DropdownField::Option> options)
{
    Field field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    Field::DropdownField control;
    control.OptionsProvider = [options = std::move(options)] { return options; };
    control.DefaultValue = defaultValue;
    control.Get = [key, defaultValue] { return ReadPreference(key, defaultValue); };
    control.Set = [key](const std::string& value) { WritePreference(key, value); };
    field.Control = std::move(control);
    return field;
}

Field ExecutableRow(std::string label, std::string tooltip, const char* key, std::string defaultValue)
{
    Field field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    Field::PathField control;
    control.PathKind = Field::PathField::Kind::File;
    control.DefaultValue = defaultValue;
    control.Get = [key, defaultValue] { return ReadPreference(key, defaultValue); };
    control.Set = [key](const std::string& value) { WritePreference(key, value); };
    field.Control = std::move(control);
    return field;
}
} // namespace

std::span<const AiAssistantSettings::ProviderChoice> AiAssistantSettings::ProviderChoices()
{
    return kProviderChoices;
}

std::string AiAssistantSettings::Provider()
{
    std::string id = ReadPreference(kPrefProvider, std::string(ClaudeSessionProvider::kId));
    return IsKnownProvider(id) ? id : std::string(ClaudeSessionProvider::kId);
}

void AiAssistantSettings::SetProvider(const std::string& providerId)
{
    if (!IsKnownProvider(providerId) || providerId == Provider())
        return;
    WritePreference(kPrefProvider, providerId);
    s_ProviderGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AiAssistantSettings::ProviderGeneration()
{
    return s_ProviderGeneration.load(std::memory_order_relaxed);
}

std::string AiAssistantSettings::Model(std::string_view providerId)
{
    if (providerId == ClaudeSessionProvider::kId)
    {
        std::string model = ReadPreference(kPrefClaudeSessionModel, kClaudeSessionDefaultModel);
        return model == kClaudeSessionDefaultModel ? std::string() : model;
    }
    if (providerId == ClaudeApiProvider::kId)
        return ReadPreference(kPrefClaudeApiModel, std::string(ClaudeApiProvider::kDefaultModel));
    if (providerId == CodexSessionProvider::kId)
        return ReadPreference(kPrefCodexModel, {});
    return {};
}

std::string AiAssistantSettings::ClaudeExecutable()
{
    return ReadPreference(kPrefClaudeExecutable, std::string(ClaudeSessionProvider::kDefaultExecutable));
}

std::string AiAssistantSettings::CodexExecutable()
{
    return ReadPreference(kPrefCodexExecutable, {});
}

std::string AiAssistantSettings::NodeExecutable()
{
    return ReadPreference(kPrefNodeExecutable, kDefaultNodeExecutable);
}

AssistantMode AiAssistantSettings::LastMode()
{
    return ParseAssistantMode(ReadPreference(kPrefLastMode, {})).value_or(kInitialMode);
}

void AiAssistantSettings::SetLastMode(AssistantMode mode)
{
    WritePreference(kPrefLastMode, std::string(DescribeAssistantMode(mode).Id));
}

std::optional<AssistantMode> AiAssistantSettings::SessionMode(std::string_view providerId,
                                                              const std::filesystem::path& projectRoot,
                                                              std::string_view sessionId)
{
    if (projectRoot.empty() || sessionId.empty())
        return std::nullopt;
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    std::string stored;
    settings.TryGetString(SessionModeKey(providerId), stored);
    const std::size_t colon = stored.find(':');
    if (colon == std::string::npos || std::string_view(stored).substr(colon + 1) != sessionId)
        return std::nullopt;
    return ParseAssistantMode(std::string_view(stored).substr(0, colon));
}

void AiAssistantSettings::SetSessionMode(std::string_view providerId, const std::filesystem::path& projectRoot,
                                         std::string_view sessionId, AssistantMode mode)
{
    if (projectRoot.empty() || sessionId.empty())
        return;
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    const std::string key = SessionModeKey(providerId);
    const std::string value = std::string(DescribeAssistantMode(mode).Id) + ":" + std::string(sessionId);
    std::string stored;
    if (settings.TryGetString(key, stored) && stored == value)
        return;
    settings.SetString(key, value);
    settings.Save();
}

std::string AiAssistantSettings::LastSession(std::string_view providerId, const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return {};
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    std::string sessionId;
    settings.TryGetString(SessionKey(providerId), sessionId);
    return sessionId;
}

void AiAssistantSettings::SetLastSession(std::string_view providerId, const std::filesystem::path& projectRoot,
                                         const std::string& sessionId)
{
    if (projectRoot.empty())
        return;
    std::lock_guard lock(s_ProjectSettingsMutex);
    Editor::SettingsStore settings = Editor::OpenUserProjectSettings(projectRoot);
    settings.Load();
    const std::string key = SessionKey(providerId);
    if (sessionId.empty())
    {
        if (!settings.Remove(key))
            return;
    }
    else
    {
        std::string stored;
        if (settings.TryGetString(key, stored) && stored == sessionId)
            return;
        settings.SetString(key, sessionId);
    }
    settings.Save();
}

Editor::SettingsCategoryDescriptor AiAssistantSettings::BuildCategory()
{
    Editor::SettingsCategoryDescriptor category;
    category.CategoryId = std::string(kCategoryId);
    category.Title = "AI Assistant";
    category.Group = Editor::SettingsCategoryGroup::UserSettings;
    category.TreeRowClass = "ai-assistant-row";
    category.SearchKeywords = "ai assistant agent claude codex model login session provider";
    category.Description =
        "Claude (local session) and Codex (local session) run your own claude and codex logins on this "
        "machine. Claude (API) reads ANTHROPIC_API_KEY from the editor's environment when a turn starts: set "
        "it before starting the editor. The editor never stores a key.";

    std::vector<Field::DropdownField::Option> providers;
    for (const ProviderChoice& choice : kProviderChoices)
        providers.push_back({std::string(choice.Id), std::string(choice.Label)});
    Field provider = DropdownRow("Provider", "The connection the AI Assistant panel talks to.", kPrefProvider,
                                 std::string(ClaudeSessionProvider::kId), std::move(providers));
    // Through SetProvider, so an open panel follows the choice.
    std::get<Field::DropdownField>(provider.Control).Set = [](const std::string& value) { SetProvider(value); };
    category.Fields.push_back(std::move(provider));
    category.Fields.push_back(DropdownRow(
        "Claude session model", "The model a Claude (local session) turn asks for.", kPrefClaudeSessionModel,
        kClaudeSessionDefaultModel,
        {{kClaudeSessionDefaultModel, "Default"}, {"fable", "Fable"}, {"opus", "Opus"}, {"sonnet", "Sonnet"}}));
    category.Fields.push_back(DropdownRow(
        "Claude API model", "The model a Claude (API) turn asks for.", kPrefClaudeApiModel,
        std::string(ClaudeApiProvider::kDefaultModel),
        {{"claude-opus-5-5", "Claude Opus 5.5"},
         {"claude-sonnet-5-5", "Claude Sonnet 5.5"},
         {"claude-haiku-4-5", "Claude Haiku 4.5"}}));

    Field codexModel;
    codexModel.Label = "Codex model";
    codexModel.Tooltip = "The model a Codex (local session) turn asks for, as Codex names it; empty uses "
                         "Codex's own default.";
    Field::StringField codexModelControl;
    codexModelControl.Get = [] { return ReadPreference(kPrefCodexModel, {}); };
    codexModelControl.Set = [](const std::string& value) { WritePreference(kPrefCodexModel, value); };
    codexModel.Control = std::move(codexModelControl);
    category.Fields.push_back(std::move(codexModel));

    std::vector<Field::DropdownField::Option> modes;
    for (const AssistantModeChoice& choice : AssistantModeChoices())
        modes.push_back({std::string(choice.Id), std::string(choice.Label)});
    category.Fields.push_back(DropdownRow(
        "Mode for new conversations",
        "What the assistant may do in the editor when a new conversation starts; the panel's mode control "
        "changes it, and a resumed session keeps its own.",
        kPrefLastMode, std::string(DescribeAssistantMode(kInitialMode).Id), std::move(modes)));

    category.Fields.push_back(ExecutableRow(
        "Claude executable", "The claude program: a name found on PATH or a full path to claude.exe.",
        kPrefClaudeExecutable, std::string(ClaudeSessionProvider::kDefaultExecutable)));
    category.Fields.push_back(ExecutableRow(
        "Codex executable", "The full path to the codex program; Codex is usually not on PATH.",
        kPrefCodexExecutable, {}));
    category.Fields.push_back(ExecutableRow(
        "Node executable",
        "The node program that runs the editor's tools for the assistant (Node 20 or later): a name found on "
        "PATH or a full path to node.exe.",
        kPrefNodeExecutable, kDefaultNodeExecutable));

    Field login;
    login.Label = "Login status";
    login.Tooltip = "Asks the chosen local session's CLI whether it is logged in.";
    login.Control = Field::CustomField{[] { return std::unique_ptr<UIElement>(std::make_unique<AiAssistantLoginStatusRow>()); }};
    category.Fields.push_back(std::move(login));
    return category;
}

void AiAssistantSettings::Register()
{
    Editor::EditorSettingsRegistry::Get().RegisterCategory(BuildCategory());
}
float AiAssistantSettings::TextSize()
{
    const std::string stored = ReadPreference(kPrefTextSize, {});
    char* end = nullptr;
    const float px = stored.empty() ? kDefaultTextSizePx : std::strtof(stored.c_str(), &end);
    if (end == stored.c_str() || !std::isfinite(px))
        return kDefaultTextSizePx;
    return std::clamp(px, kDefaultTextSizePx, kMaxTextSizePx);
}

void AiAssistantSettings::SetTextSize(float px)
{
    const float clamped = std::clamp(px, kDefaultTextSizePx, kMaxTextSizePx);
    WritePreference(kPrefTextSize, std::to_string(static_cast<int>(std::lround(clamped))));
}

} // namespace GameEngine
