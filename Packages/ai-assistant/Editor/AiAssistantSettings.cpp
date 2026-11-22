#include "AiAssistantSettings.h"

#include "ModelDisplayName.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "Providers/HarnessSessionProvider.h"
#include "Providers/OpenAiCompatibleProvider.h"
#include "Providers/TensorFoldConnection.h"
#include "Setup/AiAssistantSetupPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr const char* kPrefProvider = "aiAssistant.provider";
constexpr const char* kPrefClaudeSessionModel = "aiAssistant.claudeSessionModel";
constexpr const char* kPrefClaudeApiModel = "aiAssistant.claudeApiModel";
constexpr const char* kPrefCodexModel = "aiAssistant.codexModel";
constexpr const char* kPrefEndpointModel = "aiAssistant.endpointModel";
constexpr const char* kPrefEndpointUrl = "aiAssistant.endpointUrl";
constexpr const char* kPrefTensorFoldUrl = "aiAssistant.tensorFoldUrl";
constexpr const char* kPrefClaudeSessionEffort = "aiAssistant.claudeSessionEffort";
constexpr const char* kPrefClaudeExecutable = "aiAssistant.claudeExecutable";
constexpr const char* kPrefCodexExecutable = "aiAssistant.codexExecutable";
constexpr const char* kPrefHarnessExecutable = "aiAssistant.harnessExecutable";
constexpr const char* kPrefNodeExecutable = "aiAssistant.nodeExecutable";
constexpr const char* kPrefLastMode = "aiAssistant.lastMode";
constexpr const char* kPrefTextSize = "aiAssistant.textSize";
constexpr const char* kPrefHoldToSpeak = "aiAssistant.holdToSpeak";
constexpr const char* kPrefPromptMonospace = "aiAssistant.promptMonospace";
constexpr const char* kPrefSendWhenMicStops = "aiAssistant.sendWhenMicStops";
constexpr const char* kDefaultNodeExecutable = "node";
constexpr AssistantMode kInitialMode = AssistantMode::Auto;
// The effort a Claude session turn asks for until the user chooses one.
constexpr std::string_view kInitialClaudeSessionEffort = ClaudeSessionProvider::kEffortLevels[2];

std::atomic<uint64_t> s_ProviderGeneration{0};
std::atomic<uint64_t> s_ModelGeneration{0};
std::atomic<uint64_t> s_SetupGeneration{0};
std::atomic<uint64_t> s_PromptFontGeneration{0};

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

// What detection found, searched once and again after Check again.
struct Detection
{
    std::optional<BackendDetection::Inputs> TestInputs;
    bool Searched = false;
    std::optional<BackendDetection::Found> Claude;
    std::optional<BackendDetection::Found> Codex;
    std::optional<BackendDetection::Found> Harness;
    std::optional<BackendDetection::Found> Node;
};
std::mutex s_DetectionMutex;
Detection s_Detection;

// Searches when nothing was searched since the last Check again. Call with s_DetectionMutex held.
const Detection& Detected()
{
    if (!s_Detection.Searched)
    {
        const BackendDetection::Inputs inputs = s_Detection.TestInputs.value_or(BackendDetection::CurrentInputs());
        s_Detection.Claude = BackendDetection::FindProgram(ClaudeSessionProvider::kDefaultExecutable, inputs);
        s_Detection.Codex = BackendDetection::FindProgram(CodexSessionProvider::kDefaultExecutable, inputs);
        s_Detection.Harness = BackendDetection::FindProgram(HarnessSessionProvider::kDefaultExecutable, inputs);
        s_Detection.Node = BackendDetection::FindProgram(kDefaultNodeExecutable, inputs);
        s_Detection.Searched = true;
    }
    return s_Detection;
}

// The connection detection picks while the user has chosen none.
std::string DetectedProvider()
{
    {
        std::lock_guard lock(s_DetectionMutex);
        const Detection& detected = Detected();
        if (detected.Claude)
            return std::string(ClaudeSessionProvider::kId);
        if (detected.Codex)
            return std::string(CodexSessionProvider::kId);
        if (detected.Harness)
            return std::string(HarnessSessionProvider::kId);
    }
    const char* key = std::getenv(std::string(ApiKeyStore::kVariable).c_str());
    return std::string(key && *key ? ClaudeApiProvider::kId : ClaudeSessionProvider::kId);
}

// What detection found for `programName`; null for a program it does not look for.
const std::optional<BackendDetection::Found>* DetectedProgram(const Detection& detected, std::string_view programName)
{
    if (programName == ClaudeSessionProvider::kDefaultExecutable)
        return &detected.Claude;
    if (programName == CodexSessionProvider::kDefaultExecutable)
        return &detected.Codex;
    if (programName == HarnessSessionProvider::kDefaultExecutable)
        return &detected.Harness;
    if (programName == kDefaultNodeExecutable)
        return &detected.Node;
    return nullptr;
}

// An executable never set or set empty is the program's name when PATH finds it, else the full
// path where detection found it, else the name.
std::string ReadExecutable(const char* key, std::string_view programName)
{
    std::string executable = ReadPreference(key, {});
    if (!executable.empty())
        return executable;
    std::lock_guard lock(s_DetectionMutex);
    const std::optional<BackendDetection::Found>* found = DetectedProgram(Detected(), programName);
    if (found && *found && !(*found)->OnPath)
        return (*found)->Program.string();
    return std::string(programName);
}

// Stores an executable and moves s_SetupGeneration when it changed, so the setup is checked again.
void WriteExecutable(const char* key, const std::string& current, const std::string& value)
{
    if (value == current)
        return;
    WritePreference(key, value);
    s_SetupGeneration.fetch_add(1, std::memory_order_relaxed);
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

constexpr AiAssistantSettings::AssistantChoice kAssistantChoices[] = {
    {"Claude", "Anthropic's assistant. Can act in the editor when you sign in with your subscription."},
    {"Codex", "OpenAI's assistant. On Windows it answers in the panel but cannot act in the editor."},
    {"Harness", "DeepSeek's agent app on this computer, on the model you set in it. Answers in the panel but "
                "cannot act in the editor."},
    {"TensorFold", "The model a TensorFold server serves on this computer, with or without TensorFold Bar. "
                   "Can act in the editor."},
    {"Custom", "A server you name: a model on this computer or a hosted one, through its "
               "OpenAI-compatible API. Can act in the editor when the model calls tools."},
};

// Grouped by assistant, the method chosen with the assistant first.
constexpr AiAssistantSettings::ProviderChoice kProviderChoices[] = {
    {ClaudeSessionProvider::kId, "Claude (Subscription)", "Claude", "Subscription",
     "Your Claude account, through Claude Code.", "Needs Claude Code 2.1.259 or later, logged in. Can act in the editor."},
    {ClaudeApiProvider::kId, "Claude (API key)", "Claude", "API key", "A key from the Claude Console, billed per use.",
     "Conversation only: the assistant cannot act in the editor."},
    {CodexSessionProvider::kId, "Codex (Subscription)", "Codex", "Subscription",
     "Your ChatGPT account, through Codex.",
     "Needs Codex, logged in."},
    {HarnessSessionProvider::kId, "Harness (Desktop app)", "Harness", "Desktop app",
     "DeepSeek Harness on this computer, through its dsh program.",
     "Needs DeepSeek Harness, set up with a model. Conversation only: the assistant cannot act in the editor."},
    {TensorFoldConnection::kId, "TensorFold (Local)", "TensorFold", "Local",
     "A TensorFold server on this computer: TensorFold Bar's, or one you started, whichever model it serves.",
     "Needs a TensorFold server running a model, at 127.0.0.1:8080 unless you change the URL, and Node.js 20 "
     "or later for the editor's tools."},
    {OpenAiCompatibleProvider::kId, "Custom (OpenAI-compatible)", "Custom", "OpenAI-compatible",
     "Any server with an OpenAI-compatible chat API, such as Ollama, LM Studio, vLLM or llama.cpp.",
     "Needs the server's URL and model, a key only when the server asks for one, and Node.js 20 or later "
     "for the editor's tools, which need a model that calls tools."},
};

// The CLI's family aliases, each the latest model of its family, the initial choice
// first; the version that answered is read from the turn (SetAnsweredModel).
constexpr AiAssistantSettings::Choice kClaudeSessionModels[] = {
    {"opus", "Opus (latest)"},
    {"fable", "Fable (latest)"},
    {"sonnet", "Sonnet (latest)"},
    {"haiku", "Haiku (latest)"},
};

// The API takes exact ids; resolving a family at run time is issue #3542.
constexpr AiAssistantSettings::Choice kClaudeApiModels[] = {
    {ClaudeApiProvider::kDefaultModel, "Opus 5.5"},
    {"claude-sonnet-5-5", "Sonnet 5.5"},
    {"claude-haiku-4-5", "Haiku 4.5"},
};

// ClaudeSessionProvider::kEffortLevels in their order.
constexpr AiAssistantSettings::Choice kClaudeSessionEfforts[] = {
    {ClaudeSessionProvider::kEffortLevels[0], "Low"},
    {ClaudeSessionProvider::kEffortLevels[1], "Medium"},
    {ClaudeSessionProvider::kEffortLevels[2], "High"},
    {ClaudeSessionProvider::kEffortLevels[3], "Extra high"},
    {ClaudeSessionProvider::kEffortLevels[4], "Max"},
};
static_assert(std::size(kClaudeSessionEfforts) == std::size(ClaudeSessionProvider::kEffortLevels));

// The models that answered, by "<providerId>\n<model asked for>"; this process only.
std::mutex s_AnsweredModelsMutex;
std::vector<std::pair<std::string, std::string>> s_AnsweredModels;

std::string AnsweredKey(std::string_view providerId, std::string_view modelId)
{
    return std::string(providerId) + "\n" + std::string(modelId);
}

// Says once per process that a stored value is no longer offered, and what is used.
void ReportUnknownOnce(std::atomic<bool>& reported, std::string_view what, std::string_view stored,
                       std::string_view used)
{
    if (!reported.exchange(true))
        Logger::Log::Warning("AI Assistant: the stored {} '{}' is no longer offered; using '{}'.", what, stored,
                             used);
}

std::atomic<bool> s_ReportedUnknownModel{false};
std::atomic<bool> s_ReportedUnknownEffort{false};

// The preference that holds the model of a connection with a ModelChoices() table.
const char* ModelKey(std::string_view providerId)
{
    if (providerId == ClaudeSessionProvider::kId)
        return kPrefClaudeSessionModel;
    if (providerId == ClaudeApiProvider::kId)
        return kPrefClaudeApiModel;
    return nullptr;
}

bool Contains(std::span<const AiAssistantSettings::Choice> choices, std::string_view id)
{
    return std::any_of(choices.begin(), choices.end(),
                       [id](const AiAssistantSettings::Choice& choice) { return choice.Id == id; });
}

const AiAssistantSettings::ProviderChoice* FindProviderChoice(std::string_view id)
{
    for (const AiAssistantSettings::ProviderChoice& choice : kProviderChoices)
        if (choice.Id == id)
            return &choice;
    return nullptr;
}

bool IsKnownProvider(std::string_view id)
{
    return FindProviderChoice(id) != nullptr;
}

} // namespace

std::span<const AiAssistantSettings::AssistantChoice> AiAssistantSettings::AssistantChoices()
{
    return kAssistantChoices;
}

std::span<const AiAssistantSettings::ProviderChoice> AiAssistantSettings::ProviderChoices()
{
    return kProviderChoices;
}

std::vector<AiAssistantSettings::ProviderChoice> AiAssistantSettings::MethodChoices(std::string_view assistant)
{
    std::vector<ProviderChoice> methods;
    for (const ProviderChoice& choice : kProviderChoices)
        if (choice.Assistant == assistant)
            methods.push_back(choice);
    return methods;
}

const AiAssistantSettings::ProviderChoice& AiAssistantSettings::ProviderChoiceOf(std::string_view providerId)
{
    const ProviderChoice* choice = FindProviderChoice(providerId);
    return choice ? *choice : *FindProviderChoice(ClaudeSessionProvider::kId);
}

void AiAssistantSettings::SetAssistant(std::string_view assistant)
{
    if (ProviderChoiceOf(Provider()).Assistant == assistant)
        return;
    const std::vector<ProviderChoice> methods = MethodChoices(assistant);
    if (!methods.empty())
        SetProvider(std::string(methods.front().Id));
}

std::string AiAssistantSettings::Provider()
{
    std::string id = ReadPreference(kPrefProvider, {});
    if (id.empty())
        return DetectedProvider();
    return IsKnownProvider(id) ? id : std::string(ClaudeSessionProvider::kId);
}

void AiAssistantSettings::SetProvider(const std::string& providerId)
{
    if (!IsKnownProvider(providerId) || providerId == Provider())
        return;
    WritePreference(kPrefProvider, providerId);
    s_ProviderGeneration.fetch_add(1, std::memory_order_relaxed);
    s_SetupGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AiAssistantSettings::ProviderGeneration()
{
    return s_ProviderGeneration.load(std::memory_order_relaxed);
}

std::span<const AiAssistantSettings::Choice> AiAssistantSettings::ModelChoices(std::string_view providerId)
{
    if (providerId == ClaudeSessionProvider::kId)
        return kClaudeSessionModels;
    if (providerId == ClaudeApiProvider::kId)
        return kClaudeApiModels;
    return {};
}

std::string AiAssistantSettings::Model(std::string_view providerId)
{
    if (providerId == CodexSessionProvider::kId)
        return ReadPreference(kPrefCodexModel, {});
    if (providerId == OpenAiCompatibleProvider::kId)
        return ReadPreference(kPrefEndpointModel, {});
    const char* key = ModelKey(providerId);
    if (!key)
        return {};
    const std::string first(ModelChoices(providerId).front().Id);
    const std::string model = ReadPreference(key, first);
    if (Contains(ModelChoices(providerId), model))
        return model;
    ReportUnknownOnce(s_ReportedUnknownModel, "model", model, first);
    return first;
}

void AiAssistantSettings::SetModel(std::string_view providerId, std::string_view modelId)
{
    if (providerId == CodexSessionProvider::kId)
    {
        if (modelId == Model(providerId))
            return;
        WritePreference(kPrefCodexModel, std::string(modelId));
        s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (providerId == OpenAiCompatibleProvider::kId)
    {
        if (modelId == Model(providerId))
            return;
        WritePreference(kPrefEndpointModel, std::string(modelId));
        s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
        s_SetupGeneration.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const char* key = ModelKey(providerId);
    if (!key || !Contains(ModelChoices(providerId), modelId) || modelId == Model(providerId))
        return;
    WritePreference(key, std::string(modelId));
    s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
}

std::string AiAssistantSettings::ModelLabel(std::string_view providerId, std::string_view modelId)
{
    if (const std::string answered = AnsweredModel(providerId, modelId); !answered.empty())
        return ModelDisplayName(answered);
    for (const Choice& choice : ModelChoices(providerId))
        if (choice.Id == modelId)
            return std::string(choice.Label);
    return ModelDisplayName(modelId);
}

std::string AiAssistantSettings::AnsweredModel(std::string_view providerId, std::string_view modelId)
{
    const std::string key = AnsweredKey(providerId, modelId);
    std::lock_guard lock(s_AnsweredModelsMutex);
    for (const auto& [asked, answered] : s_AnsweredModels)
        if (asked == key)
            return answered;
    return {};
}

void AiAssistantSettings::SetAnsweredModel(std::string_view providerId, std::string_view modelId,
                                           std::string_view answeredModel)
{
    const std::string key = AnsweredKey(providerId, modelId);
    {
        std::lock_guard lock(s_AnsweredModelsMutex);
        auto entry = std::find_if(s_AnsweredModels.begin(), s_AnsweredModels.end(),
                                  [&key](const auto& asked) { return asked.first == key; });
        if (entry == s_AnsweredModels.end())
            s_AnsweredModels.emplace_back(key, answeredModel);
        else if (entry->second != answeredModel)
            entry->second = answeredModel;
        else
            return;
    }
    s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
}

std::span<const AiAssistantSettings::Choice> AiAssistantSettings::EffortChoices(std::string_view providerId)
{
    if (providerId == ClaudeSessionProvider::kId)
        return kClaudeSessionEfforts;
    return {};
}

std::string AiAssistantSettings::Effort(std::string_view providerId)
{
    if (providerId != ClaudeSessionProvider::kId)
        return {};
    const std::string initial(kInitialClaudeSessionEffort);
    const std::string effort = ReadPreference(kPrefClaudeSessionEffort, initial);
    if (Contains(kClaudeSessionEfforts, effort))
        return effort;
    ReportUnknownOnce(s_ReportedUnknownEffort, "effort", effort, initial);
    return initial;
}

void AiAssistantSettings::SetEffort(std::string_view providerId, std::string_view effortId)
{
    if (!Contains(EffortChoices(providerId), effortId) || effortId == Effort(providerId))
        return;
    WritePreference(kPrefClaudeSessionEffort, std::string(effortId));
    s_ModelGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AiAssistantSettings::SetupGeneration()
{
    return s_SetupGeneration.load(std::memory_order_relaxed);
}

void AiAssistantSettings::CheckSetupAgain()
{
    const std::string before = Provider();
    {
        std::lock_guard lock(s_DetectionMutex);
        s_Detection.Searched = false;
    }
    if (Provider() != before)
        s_ProviderGeneration.fetch_add(1, std::memory_order_relaxed);
    s_SetupGeneration.fetch_add(1, std::memory_order_relaxed);
}

void AiAssistantSettings::UseDetectionInputsForTesting(std::optional<BackendDetection::Inputs> inputs)
{
    std::lock_guard lock(s_DetectionMutex);
    s_Detection.TestInputs = std::move(inputs);
    s_Detection.Searched = false;
}

std::string AiAssistantSettings::EndpointUrl()
{
    return ReadPreference(kPrefEndpointUrl, {});
}

std::string AiAssistantSettings::TensorFoldUrl()
{
    const std::string url = ReadPreference(kPrefTensorFoldUrl, {});
    return url.empty() ? std::string(TensorFoldConnection::kDefaultBaseUrl) : url;
}

void AiAssistantSettings::SetTensorFoldUrl(const std::string& url)
{
    if (url == ReadPreference(kPrefTensorFoldUrl, {}))
        return;
    WritePreference(kPrefTensorFoldUrl, url);
    s_SetupGeneration.fetch_add(1, std::memory_order_relaxed);
}

void AiAssistantSettings::SetEndpointUrl(const std::string& url)
{
    if (url == EndpointUrl())
        return;
    WritePreference(kPrefEndpointUrl, url);
    s_SetupGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AiAssistantSettings::ModelGeneration()
{
    return s_ModelGeneration.load(std::memory_order_relaxed);
}

std::string AiAssistantSettings::ClaudeExecutable()
{
    return ReadExecutable(kPrefClaudeExecutable, ClaudeSessionProvider::kDefaultExecutable);
}

std::string AiAssistantSettings::CodexExecutable()
{
    return ReadExecutable(kPrefCodexExecutable, CodexSessionProvider::kDefaultExecutable);
}

std::string AiAssistantSettings::HarnessExecutable()
{
    return ReadExecutable(kPrefHarnessExecutable, HarnessSessionProvider::kDefaultExecutable);
}

std::string AiAssistantSettings::NodeExecutable()
{
    return ReadExecutable(kPrefNodeExecutable, kDefaultNodeExecutable);
}

void AiAssistantSettings::SetClaudeExecutable(const std::string& executable)
{
    WriteExecutable(kPrefClaudeExecutable, ClaudeExecutable(), executable);
}

void AiAssistantSettings::SetCodexExecutable(const std::string& executable)
{
    WriteExecutable(kPrefCodexExecutable, CodexExecutable(), executable);
}

void AiAssistantSettings::SetHarnessExecutable(const std::string& executable)
{
    WriteExecutable(kPrefHarnessExecutable, HarnessExecutable(), executable);
}

void AiAssistantSettings::SetNodeExecutable(const std::string& executable)
{
    WriteExecutable(kPrefNodeExecutable, NodeExecutable(), executable);
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

bool AiAssistantSettings::PromptMonospace()
{
    return ReadPreference(kPrefPromptMonospace, "0") == "1";
}

// Writes only a change: the settings page calls Set with the stored value when it opens.
void AiAssistantSettings::SetPromptMonospace(bool monospace)
{
    if (PromptMonospace() == monospace)
        return;
    WritePreference(kPrefPromptMonospace, monospace ? "1" : "0");
    s_PromptFontGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AiAssistantSettings::PromptFontGeneration()
{
    return s_PromptFontGeneration.load(std::memory_order_relaxed);
}

bool AiAssistantSettings::HoldToSpeak()
{
    return ReadPreference(kPrefHoldToSpeak, "0") == "1";
}

// Writes only a change: the settings page calls Set with the stored value when it opens.
void AiAssistantSettings::SetHoldToSpeak(bool hold)
{
    if (HoldToSpeak() != hold)
        WritePreference(kPrefHoldToSpeak, hold ? "1" : "0");
}

bool AiAssistantSettings::SendWhenMicStops()
{
    return ReadPreference(kPrefSendWhenMicStops, "0") == "1";
}

void AiAssistantSettings::SetSendWhenMicStops(bool send)
{
    if (SendWhenMicStops() != send)
        WritePreference(kPrefSendWhenMicStops, send ? "1" : "0");
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
    // The page's own rows are not registry fields, so their names are search words here.
    category.SearchKeywords =
        "ai assistant agent claude codex subscription api key provider setup check login session model effort "
        "codex model mode for new conversations claude code executable codex executable node executable "
        "hold the microphone to speak send when the microphone stops custom endpoint url openai compatible "
        "local model server ollama lm studio vllm llama.cpp tensorfold deepseek harness dsh monospaced prompt font";
    category.CreateContent = [] { return std::unique_ptr<UIElement>(std::make_unique<AiAssistantSetupPage>()); };
    return category;
}

void AiAssistantSettings::Register()
{
    Editor::EditorSettingsRegistry::Get().RegisterCategory(BuildCategory());
}
} // namespace GameEngine
