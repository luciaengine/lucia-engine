#include "OpenAiChatStream.h"

#include "CancelToken.h"
#include "JsonFields.h"

#include <chrono>
#include <algorithm>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// A model may think for minutes before its first token; a reply that keeps arriving is never cut.
constexpr std::chrono::milliseconds kStreamStallTimeout = std::chrono::minutes(10);
constexpr size_t kBytesPerMiB = size_t{1024} * 1024;
constexpr int kHttpUnauthorized = 401;

std::string FinishReasonNote(std::string_view reason)
{
    if (reason == "length")
        return "The reply stopped at the endpoint's token limit; ask the model to continue or for a shorter answer.";
    if (reason == "content_filter")
        return "The endpoint's content filter stopped the reply.";
    return {};
}

// The message of an OpenAI-style {"error": {"message": ...}} body, or of {"error": "..."}.
std::string DescribeError(const nlohmann::json& payload)
{
    const auto error = payload.find("error");
    if (error == payload.end())
        return {};
    if (error->is_string())
        return error->get<std::string>();
    if (error->is_object())
        return JsonFields::String(*error, "message");
    return {};
}
} // namespace

OpenAiChatStream::Round OpenAiChatStream::Run(const std::string& url,
                                              const std::unordered_map<std::string, std::string>& headers,
                                              const std::string& body,
                                              AgentTurnEvents& events,
                                              const CancelToken& cancel,
                                              std::string textPrefix)
{
    HttpClient::PostOptions options;
    options.Timeout = kStreamStallTimeout;
    options.MaxResponseBytes = kMaxResponseBytes;
    options.Cancellation = &cancel.Flag();

    OpenAiChatStream stream(events, std::move(textPrefix));
    HttpClient::HttpResponse response;
    try
    {
        response = HttpClient::PostJsonStream(url, headers, body, options,
                                              [&stream](std::string_view chunk) { stream.Feed(chunk); });
    }
    catch (const std::exception& error)
    {
        response.error = error.what();
    }
    Round round;
    round.Result = stream.End(response);
    round.ToolCalls = stream.ToolCalls();
    return round;
}

OpenAiChatStream::OpenAiChatStream(AgentTurnEvents& events, std::string textPrefix)
    : m_Events(events), m_TextPrefix(std::move(textPrefix))
{
}

void OpenAiChatStream::Feed(std::string_view bytes)
{
    m_Parser.Feed(bytes, [this](const SseEvent& event) { OnEvent(event); });
}

void OpenAiChatStream::OnEvent(const SseEvent& event)
{
    if (m_Failed || m_Done)
        return;
    if (event.Data == "[DONE]")
    {
        m_Done = true;
        return;
    }
    const nlohmann::json payload = nlohmann::json::parse(event.Data, nullptr, false);
    if (!payload.is_object())
    {
        Fail("The endpoint sent a stream chunk that is not a JSON object.");
        return;
    }
    if (const std::string error = DescribeError(payload); !error.empty())
    {
        Fail("The endpoint ended the reply with an error: " + error + ". Send the message again.");
        return;
    }
    if (const std::string model = JsonFields::String(payload, "model"); !model.empty())
        m_Result.Model = model;
    const nlohmann::json& usage = JsonFields::Object(payload, "usage");
    if (usage.contains("prompt_tokens"))
        m_Result.InputTokens = JsonFields::Count(usage, "prompt_tokens");
    if (usage.contains("completion_tokens"))
        m_Result.OutputTokens = JsonFields::Count(usage, "completion_tokens");

    const auto choices = payload.find("choices");
    if (choices == payload.end() || !choices->is_array() || choices->empty() || !choices->front().is_object())
        return;
    const nlohmann::json& choice = choices->front();
    m_Result.ModelRoundTrips = 1;
    const nlohmann::json& delta = JsonFields::Object(choice, "delta");
    std::string text = JsonFields::String(delta, "content");
    if (!text.empty())
    {
        // A later round's reply is a paragraph of its own.
        text = std::exchange(m_TextPrefix, {}) + text;
        m_Result.Text += text;
        m_Events.OnTextDelta(text);
    }
    ReadToolCalls(delta);
    if (const std::string reason = JsonFields::String(choice, "finish_reason"); !reason.empty())
        m_FinishReason = reason;
}

void OpenAiChatStream::ReadToolCalls(const nlohmann::json& delta)
{
    const auto calls = delta.find("tool_calls");
    if (calls == delta.end() || !calls->is_array())
        return;
    for (const nlohmann::json& piece : *calls)
    {
        if (!piece.is_object())
            continue;
        const uint64_t index = JsonFields::Count(piece, "index");
        auto call = std::find_if(m_ToolCalls.begin(), m_ToolCalls.end(),
                                 [index](const auto& entry) { return entry.first == index; });
        if (call == m_ToolCalls.end())
            call = m_ToolCalls.insert(m_ToolCalls.end(), {index, ToolCall{}});
        if (const std::string id = JsonFields::String(piece, "id"); !id.empty())
            call->second.Id = id;
        const nlohmann::json& function = JsonFields::Object(piece, "function");
        call->second.Name += JsonFields::String(function, "name");
        call->second.Arguments += JsonFields::String(function, "arguments");
    }
}

std::vector<OpenAiChatStream::ToolCall> OpenAiChatStream::ToolCalls() const
{
    std::vector<std::pair<uint64_t, ToolCall>> sorted = m_ToolCalls;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<ToolCall> calls;
    for (auto& [index, call] : sorted)
        if (!call.Name.empty())
            calls.push_back(std::move(call));
    return calls;
}

void OpenAiChatStream::Fail(std::string message)
{
    m_Failed = true;
    m_Result.Error = std::move(message);
}

void OpenAiChatStream::Finish(const HttpClient::HttpResponse& response)
{
    m_Events.OnFinished(End(response));
}

TurnResult OpenAiChatStream::End(const HttpClient::HttpResponse& response)
{
    TurnResult result = std::move(m_Result);
    if (response.cancelled)
    {
        result.Outcome = TurnOutcome::Stopped;
        result.Error.clear();
    }
    else if (m_Failed)
    {
        result.Outcome = TurnOutcome::Failed;
    }
    else if (response.overflowed)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = "The endpoint's reply passed " + std::to_string(kMaxResponseBytes / kBytesPerMiB) +
                       " MiB, so it was cut off. The text received so far is kept; send the message again.";
    }
    else if (!response.success)
    {
        result.Outcome = TurnOutcome::Failed;
        if (response.statusCode >= 400)
        {
            const nlohmann::json payload = nlohmann::json::parse(response.body, nullptr, false);
            const std::string detail = payload.is_object() ? DescribeError(payload) : std::string{};
            result.Error = "The endpoint refused the request (HTTP " + std::to_string(response.statusCode) +
                           (detail.empty() ? std::string{} : ", " + detail) + ").";
            if (response.statusCode == kHttpUnauthorized)
                result.Error += " Check the endpoint's key in Settings > AI Assistant.";
        }
        else
        {
            result.Error = "The endpoint request failed: " +
                           (response.error.empty() ? "HTTP " + std::to_string(response.statusCode) : response.error);
        }
    }
    else if (!m_Done && m_FinishReason.empty())
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = "The endpoint's stream ended before the reply finished. Send the message again.";
    }
    else
    {
        result.Outcome = TurnOutcome::Succeeded;
        result.Note = FinishReasonNote(m_FinishReason);
    }
    return result;
}
} // namespace GameEngine
