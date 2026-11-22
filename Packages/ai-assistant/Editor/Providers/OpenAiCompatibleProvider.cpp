#include "OpenAiCompatibleProvider.h"

#include "CancelToken.h"
#include "JsonFields.h"
#include "EditorToolBridge.h"
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

// The models <base>/models lists, in its order; empty when the server does not answer with a list.
std::vector<std::string> ListedModels(const HttpClient::HttpResponse& response)
{
    std::vector<std::string> models;
    if (!response.success)
        return models;
    const nlohmann::json payload = nlohmann::json::parse(response.body, nullptr, false);
    const auto data = payload.is_object() ? payload.find("data") : payload.end();
    if (!payload.is_object() || data == payload.end() || !data->is_array())
        return models;
    for (const nlohmann::json& entry : *data)
        if (entry.is_object())
            if (std::string id = JsonFields::String(entry, "id"); !id.empty())
                models.push_back(std::move(id));
    return models;
}

HttpClient::HttpResponse GetModels(const std::string& baseUrl, const ApiKeyStore::Key& key, const CancelToken& cancel)
{
    std::unordered_map<std::string, std::string> headers = OpenAiCompatibleProvider::BuildRequestHeaders(key.Value);
    headers.erase("accept");
    return HttpClient::Get(baseUrl + "/models", headers,
                           {.MaxResponseBytes = kMaxModelsResponseBytes, .Timeout = kAnswersTimeout,
                            .Cancellation = &cancel.Flag()});
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
// An empty `model` (ModelSource::FirstListed) passes when the server lists any model.
SetupStep AnswersStep(const std::string& baseUrl, const std::string& model, const ApiKeyStore::Key& key,
                      SetupSurface surface, const CancelToken& cancel)
{
    SetupStep step{.Name = std::string(OpenAiCompatibleProvider::kAnswersStep)};
    const HttpClient::HttpResponse response = GetModels(baseUrl, key, cancel);
    if (response.success && model.empty())
    {
        const std::vector<std::string> models = ListedModels(response);
        step.State = models.empty() ? SetupStepState::Failed : SetupStepState::Passed;
        step.Found = models.empty() ? "The endpoint answered, but lists no model."
                                    : "The endpoint answered; it serves " + models.front() + ".";
        if (models.empty())
            step.Fix = "Load a model in the server, then click Check again.";
        return step;
    }
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

OpenAiCompatibleProvider::OpenAiCompatibleProvider(ApiKeyStore keys, std::string baseUrl, std::string model,
                                                   std::string_view id, ModelSource modelSource)
    : m_Id(id), m_ModelSource(modelSource), m_Keys(std::move(keys)), m_BaseUrl(NormalizeBaseUrl(baseUrl)),
      m_Model(std::move(model))
{
}

ProviderCapabilities OpenAiCompatibleProvider::Capabilities() const
{
    return {.Streams = true, .OwnsTranscript = false, .NeedsKey = false, .CanActWithTools = true};
}

SetupStatus OpenAiCompatibleProvider::CheckSetup(const CancelToken& cancel, SetupSurface surface,
                                                 const SetupStepSink& onStep) const
{
    SetupStatus status;
    Publish(EndpointStep(m_BaseUrl, surface), onStep, status);
    if (m_ModelSource == ModelSource::Given)
        Publish(ModelStep(m_Model, surface), onStep, status);
    if (!status.Ready() || cancel.IsCancelled())
        return status;
    Publish(AnswersStep(m_BaseUrl, m_Model, m_Keys.Read(), surface, cancel), onStep, status);
    return status;
}

std::vector<SetupStep> OpenAiCompatibleProvider::SetupPlan() const
{
    std::vector<SetupStep> plan = {{.Name = std::string(kEndpointStep), .Found = "Reads the endpoint URL."}};
    if (m_ModelSource == ModelSource::Given)
        plan.push_back({.Name = std::string(kModelStep), .Found = "Reads the model name."});
    plan.push_back({.Name = std::string(kAnswersStep), .Found = "Asks the endpoint for its model list."});
    return plan;
}

void OpenAiCompatibleProvider::RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events,
                                       const CancelToken& cancel)
{
    std::string refusal;
    const std::string model = TurnModel(request, refusal, cancel);
    if (model.empty())
    {
        TurnResult result;
        result.Outcome = cancel.IsCancelled() ? TurnOutcome::Stopped : TurnOutcome::Failed;
        result.NotReady = !cancel.IsCancelled();
        result.Error = cancel.IsCancelled() ? std::string() : refusal;
        events.OnFinished(result);
        return;
    }
    if (request.Tools)
    {
        RunToolTurn(request, model, events, cancel);
        return;
    }
    events.OnFinished(OpenAiChatStream::Run(m_BaseUrl + std::string(kChatPath), BuildRequestHeaders(m_Keys.Read().Value),
                                            BuildRequestBody(request, model), events, cancel)
                          .Result);
}

void OpenAiCompatibleProvider::RunToolTurn(const AgentTurnRequest& request, const std::string& model,
                                           AgentTurnEvents& events, const CancelToken& cancel)
{
    EditorToolBridge bridge(*request.Tools);
    TurnResult total;
    if (std::string error; !bridge.Load(cancel, error))
    {
        total.Outcome = cancel.IsCancelled() ? TurnOutcome::Stopped : TurnOutcome::Failed;
        total.Error = cancel.IsCancelled() ? std::string() : error;
        events.OnFinished(total);
        return;
    }
    const std::unordered_map<std::string, std::string> headers = BuildRequestHeaders(m_Keys.Read().Value);
    nlohmann::json messages = BuildMessages(request);
    const nlohmann::json tools = bridge.Definitions();
    for (int round = 0; round < kMaxToolRounds; ++round)
    {
        nlohmann::json body = {{"model", model}, {"stream", true}, {"messages", messages}, {"tools", tools}};
        OpenAiChatStream::Round reply =
            OpenAiChatStream::Run(m_BaseUrl + std::string(kChatPath), headers,
                                  body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), events, cancel,
                                  total.Text.empty() ? std::string() : std::string("\n\n"));
        total.Text += reply.Result.Text;
        total.InputTokens += reply.Result.InputTokens;
        total.OutputTokens += reply.Result.OutputTokens;
        total.ModelRoundTrips += 1;
        if (!reply.Result.Model.empty())
            total.Model = reply.Result.Model;
        if (reply.Result.Outcome != TurnOutcome::Succeeded || reply.ToolCalls.empty())
        {
            total.Outcome = reply.Result.Outcome;
            total.Error = reply.Result.Error;
            total.Note = reply.Result.Note;
            events.OnFinished(total);
            return;
        }

        // The model's tool calls, then one answer per call, in the order it made them.
        nlohmann::json calls = nlohmann::json::array();
        for (size_t index = 0; index < reply.ToolCalls.size(); ++index)
        {
            OpenAiChatStream::ToolCall& call = reply.ToolCalls[index];
            if (call.Id.empty())
                call.Id = "call_" + std::to_string(round) + "_" + std::to_string(index);
            calls.push_back({{"id", call.Id},
                             {"type", "function"},
                             {"function", {{"name", call.Name}, {"arguments", call.Arguments}}}});
        }
        messages.push_back({{"role", "assistant"},
                            {"content", reply.Result.Text.empty() ? nlohmann::json(nullptr)
                                                                  : nlohmann::json(reply.Result.Text)},
                            {"tool_calls", std::move(calls)}});
        for (const OpenAiChatStream::ToolCall& call : reply.ToolCalls)
        {
            events.OnToolActivity({EditorToolBridge::RunsTool(call.Name, call.Arguments), call.Arguments, call.Id});
            const EditorToolBridge::Answer answer = bridge.Call(call.Name, call.Arguments, cancel);
            if (answer.IsError)
                events.OnToolFailed({.CallId = call.Id, .Error = answer.Text.substr(0, answer.Text.find('\n'))});
            messages.push_back({{"role", "tool"}, {"tool_call_id", call.Id}, {"content", answer.Text}});
            if (cancel.IsCancelled())
            {
                total.Outcome = TurnOutcome::Stopped;
                events.OnFinished(total);
                return;
            }
        }
    }
    total.Outcome = TurnOutcome::Failed;
    total.Error = "The model kept calling tools for " + std::to_string(kMaxToolRounds) +
                  " requests without answering, so the turn stopped. Send a narrower request.";
    events.OnFinished(total);
}

std::string OpenAiCompatibleProvider::TurnModel(const AgentTurnRequest& request, std::string& refusal,
                                                const CancelToken& cancel) const
{
    if (const SetupStep endpoint = EndpointStep(m_BaseUrl, SetupSurface::Panel); endpoint.State == SetupStepState::Failed)
    {
        refusal = endpoint.Found + " " + endpoint.Fix;
        return {};
    }
    if (!request.Model.empty())
        return request.Model;
    if (m_ModelSource == ModelSource::Given)
    {
        const SetupStep step = ModelStep(m_Model, SetupSurface::Panel);
        refusal = step.Found + " " + step.Fix;
        return m_Model;
    }
    const HttpClient::HttpResponse response = GetModels(m_BaseUrl, m_Keys.Read(), cancel);
    const std::vector<std::string> models = ListedModels(response);
    if (!models.empty())
        return models.front();
    refusal = response.statusCode == 0
                  ? "The endpoint at " + m_BaseUrl + " could not be reached (" + response.error +
                        "). Start the server, or correct its URL in Settings > AI Assistant."
                  : "The endpoint at " + m_BaseUrl + " lists no model. Load a model in the server.";
    return {};
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
    nlohmann::json body = {{"model", std::string(model)}, {"stream", true}, {"messages", BuildMessages(request)}};
    // Invalid UTF-8 in typed text becomes U+FFFD instead of throwing out of the turn.
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

nlohmann::json OpenAiCompatibleProvider::BuildMessages(const AgentTurnRequest& request)
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
    return messages;
}

std::unordered_map<std::string, std::string> OpenAiCompatibleProvider::BuildRequestHeaders(std::string_view key)
{
    std::unordered_map<std::string, std::string> headers{{"accept", "text/event-stream"}};
    if (!key.empty())
        headers.emplace("Authorization", "Bearer " + std::string(key));
    return headers;
}
} // namespace GameEngine
