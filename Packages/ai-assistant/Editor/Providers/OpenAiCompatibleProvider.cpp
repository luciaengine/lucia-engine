#include "OpenAiCompatibleProvider.h"

#include "CancelToken.h"
#include "JsonFields.h"
#include "OpenAiChatStream.h"

#include "Logger/Logger.h"
#include "Platform/HttpClient.h"
#include "Types/Base64.h"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
constexpr std::string_view kChatPath = "/chat/completions";
// A models list is a few kilobytes; the cap bounds a misbehaving answer.
constexpr size_t kMaxModelsResponseBytes = size_t{1024} * 1024;
// A server on this computer answers at once; a hosted one within seconds.
constexpr std::chrono::milliseconds kAnswersTimeout = std::chrono::seconds(15);
constexpr int kHttpUnauthorized = 401;
constexpr int kHttpForbidden = 403;

std::string OpenSettingsPrefix(SetupSurface surface)
{
    return surface == SetupSurface::SettingsPage ? std::string() : "Open Settings > AI Assistant and ";
}

// The fix sentence: "Type ..." on the page, "Open Settings > AI Assistant and type ..." in the panel.
std::string Fix(SetupSurface surface, std::string_view action)
{
    std::string text = OpenSettingsPrefix(surface);
    if (text.empty())
    {
        text = std::string(action);
        if (!text.empty())
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        return text;
    }
    return text + std::string(action);
}

bool IsHttpUrl(std::string_view url)
{
    return url.starts_with("http://") || url.starts_with("https://");
}

SetupStep EndpointStep(const std::string& baseUrl, SetupSurface surface)
{
    SetupStep step{.Name = std::string(OpenAiCompatibleProvider::kEndpointStep)};
    if (baseUrl.empty())
    {
        step.State = SetupStepState::Failed;
        step.Found = "No endpoint URL is set.";
        step.Fix = Fix(surface, "type the server's API URL, such as http://localhost:11434/v1.");
        return step;
    }
    if (!IsHttpUrl(baseUrl))
    {
        step.State = SetupStepState::Failed;
        step.Found = "The endpoint URL " + baseUrl + " does not start with http:// or https://.";
        step.Fix = Fix(surface, "type the server's API URL, such as http://localhost:11434/v1.");
        return step;
    }
    step.State = SetupStepState::Passed;
    step.Found = "Uses " + baseUrl + ".";
    return step;
}

SetupStep ModelStep(const std::string& model, SetupSurface surface)
{
    SetupStep step{.Name = std::string(OpenAiCompatibleProvider::kModelStep)};
    if (model.empty())
    {
        step.State = SetupStepState::Failed;
        step.Found = "No model is set.";
        step.Fix = Fix(surface, "type the model the server serves, as the server names it.");
        return step;
    }
    step.State = SetupStepState::Passed;
    step.Found = "Asks for " + model + ".";
    return step;
}

bool ListsModel(const nlohmann::json& payload, const std::string& model)
{
    const auto data = payload.find("data");
    if (data == payload.end() || !data->is_array())
        return false;
    for (const nlohmann::json& entry : *data)
        if (entry.is_object() && JsonFields::String(entry, "id") == model)
            return true;
    return false;
}

// One GET of <base>/models. A server that cannot be reached fails: nothing would answer a turn.
SetupStep AnswersStep(const std::string& baseUrl, const std::string& model, const ApiKeyStore::Key& key,
                      SetupSurface surface, const CancelToken& cancel)
{
    SetupStep step{.Name = std::string(OpenAiCompatibleProvider::kAnswersStep)};
    std::unordered_map<std::string, std::string> headers = OpenAiCompatibleProvider::BuildRequestHeaders(key.Value);
    headers.erase("accept");
    const HttpClient::HttpResponse response = HttpClient::Get(
        baseUrl + "/models", headers,
        {.MaxResponseBytes = kMaxModelsResponseBytes, .Timeout = kAnswersTimeout, .Cancellation = &cancel.Flag()});
    if (response.success)
    {
        const nlohmann::json payload = nlohmann::json::parse(response.body, nullptr, false);
        if (payload.is_object() && payload.contains("data") && !ListsModel(payload, model))
        {
            step.State = SetupStepState::Warning;
            step.Found = "The endpoint answered, but its model list does not name " + model + ".";
            step.Fix = Fix(surface, "check the model name; a server that serves it under another name still answers.");
            return step;
        }
        step.State = SetupStepState::Passed;
        step.Found = "The endpoint answered.";
        return step;
    }
    if (response.statusCode == kHttpUnauthorized || response.statusCode == kHttpForbidden)
    {
        step.State = SetupStepState::Failed;
        step.Found = "The endpoint refused the request (HTTP " + std::to_string(response.statusCode) + ").";
        step.Fix = Fix(surface, key.Value.empty() ? "save the key the server needs."
                                                  : "save a key the server accepts.");
        return step;
    }
    if (response.statusCode == 0)
    {
        step.State = SetupStepState::Failed;
        step.Found = "The endpoint could not be reached (" + response.error + ").";
        step.Fix = "Start the server or correct the URL, then click Check again.";
        return step;
    }
    step.State = SetupStepState::Warning;
    step.Found = "The endpoint answered HTTP " + std::to_string(response.statusCode) +
                 " for its model list, so the model is not checked.";
    step.Fix = "Send a prompt to try the endpoint.";
    return step;
}

void Publish(const SetupStep& step, const SetupStepSink& onStep, SetupStatus& status)
{
    Logger::Log::Info("AI Assistant setup: {}: {}", step.Name, step.Found);
    if (onStep)
        onStep(step);
    status.Steps.push_back(step);
}
} // namespace

OpenAiCompatibleProvider::OpenAiCompatibleProvider(ApiKeyStore keys, std::string baseUrl, std::string model)
    : m_Keys(std::move(keys)), m_BaseUrl(NormalizeBaseUrl(baseUrl)), m_Model(std::move(model))
{
}

ProviderCapabilities OpenAiCompatibleProvider::Capabilities() const
{
    return {.Streams = true, .OwnsTranscript = false, .NeedsKey = false, .CanActWithTools = false};
}

SetupStatus OpenAiCompatibleProvider::CheckSetup(const CancelToken& cancel, SetupSurface surface,
                                                 const SetupStepSink& onStep) const
{
    SetupStatus status;
    Publish(EndpointStep(m_BaseUrl, surface), onStep, status);
    Publish(ModelStep(m_Model, surface), onStep, status);
    if (!status.Ready() || cancel.IsCancelled())
        return status;
    Publish(AnswersStep(m_BaseUrl, m_Model, m_Keys.Read(), surface, cancel), onStep, status);
    return status;
}

std::vector<SetupStep> OpenAiCompatibleProvider::SetupPlan() const
{
    return {{.Name = std::string(kEndpointStep), .Found = "Reads the endpoint URL."},
            {.Name = std::string(kModelStep), .Found = "Reads the model name."},
            {.Name = std::string(kAnswersStep), .Found = "Asks the endpoint for its model list."}};
}

void OpenAiCompatibleProvider::RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events,
                                       const CancelToken& cancel)
{
    const std::string model = request.Model.empty() ? m_Model : request.Model;
    for (const SetupStep& step : {EndpointStep(m_BaseUrl, SetupSurface::Panel), ModelStep(model, SetupSurface::Panel)})
    {
        if (step.State != SetupStepState::Failed)
            continue;
        TurnResult result;
        result.Outcome = TurnOutcome::Failed;
        result.NotReady = true;
        result.Error = step.Found + " " + step.Fix;
        events.OnFinished(result);
        return;
    }
    OpenAiChatStream::Run(m_BaseUrl + std::string(kChatPath), BuildRequestHeaders(m_Keys.Read().Value),
                          BuildRequestBody(request, model), events, cancel);
}

std::string OpenAiCompatibleProvider::NormalizeBaseUrl(std::string_view baseUrl)
{
    while (!baseUrl.empty() && (baseUrl.back() == ' ' || baseUrl.back() == '\t'))
        baseUrl.remove_suffix(1);
    while (!baseUrl.empty() && (baseUrl.front() == ' ' || baseUrl.front() == '\t'))
        baseUrl.remove_prefix(1);
    while (!baseUrl.empty() && baseUrl.back() == '/')
        baseUrl.remove_suffix(1);
    if (baseUrl.ends_with(kChatPath))
        baseUrl.remove_suffix(kChatPath.size());
    while (!baseUrl.empty() && baseUrl.back() == '/')
        baseUrl.remove_suffix(1);
    return std::string(baseUrl);
}

std::string OpenAiCompatibleProvider::BuildRequestBody(const AgentTurnRequest& request, std::string_view model)
{
    nlohmann::json messages = nlohmann::json::array();
    if (!request.SystemPrompt.empty())
        messages.push_back({{"role", "system"}, {"content", request.SystemPrompt}});
    for (const ConversationMessage& message : request.History)
    {
        if (message.Text.empty())
            continue;
        const bool user = message.Role == MessageRole::User;
        messages.push_back({{"role", user ? "user" : "assistant"}, {"content", user ? message.SentText() : message.Text}});
    }
    if (request.Images.empty())
    {
        messages.push_back({{"role", "user"}, {"content", request.UserText}});
    }
    else
    {
        nlohmann::json content = nlohmann::json::array();
        for (const PromptFile& image : request.Images)
            content.push_back({{"type", "image_url"},
                               {"image_url", {{"url", "data:" + image.MediaType + ";base64," + Base64Encode(image.Bytes)}}}});
        content.push_back({{"type", "text"}, {"text", request.UserText}});
        messages.push_back({{"role", "user"}, {"content", std::move(content)}});
    }
    nlohmann::json body = {{"model", std::string(model)}, {"stream", true}, {"messages", std::move(messages)}};
    // Invalid UTF-8 in typed text becomes U+FFFD instead of throwing out of the turn.
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::unordered_map<std::string, std::string> OpenAiCompatibleProvider::BuildRequestHeaders(std::string_view key)
{
    std::unordered_map<std::string, std::string> headers{{"accept", "text/event-stream"}};
    if (!key.empty())
        headers.emplace("Authorization", "Bearer " + std::string(key));
    return headers;
}
} // namespace GameEngine
