#pragma once

#include "IAgentProvider.h"
#include "SseParser.h"

#include "Platform/HttpClient.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
class CancelToken;

/// One OpenAI-compatible chat completions turn as it streams (POST .../chat/completions with
/// stream true): each server-sent event's data is a chunk whose `choices[0].delta.content` text
/// becomes OnTextDelta in arrival order; `choices[0].finish_reason` ends the reply (a "length" or
/// "content_filter" reason becomes the turn's Note); `usage` carries the token counts; `model`
/// names the model that answered; `data: [DONE]` ends the stream. `choices[0].delta.tool_calls`
/// pieces, keyed by their index, add up to the round's tool calls. A chunk with an `error` object
/// fails the turn with its message; a chunk that is not JSON fails it too.
class OpenAiChatStream
{
public:
    /// The response body cap: the same size as the Claude API stream's.
    static constexpr size_t kMaxResponseBytes = size_t{16384} * 512;

    /// A tool call the model made, whole.
    struct ToolCall
    {
        std::string Id;
        std::string Name;
        /// The arguments as the model wrote them: a JSON object as text.
        std::string Arguments;
    };

    /// One request's reply: the turn's result as End() makes it, and the tool calls it asked for.
    struct Round
    {
        TurnResult Result;
        std::vector<ToolCall> ToolCalls;
    };

    /// Posts `body` to `url` with `headers` and streams the reply's text into `events`, with
    /// `textPrefix` before its first text; returns the round without calling OnFinished, so a tool
    /// loop can continue the turn. `cancel` stops the transfer.
    static Round Run(const std::string& url,
                     const std::unordered_map<std::string, std::string>& headers,
                     const std::string& body,
                     AgentTurnEvents& events,
                     const CancelToken& cancel,
                     std::string textPrefix = {});

    explicit OpenAiChatStream(AgentTurnEvents& events, std::string textPrefix = {});

    /// Consumes the next response bytes, split anywhere.
    void Feed(std::string_view bytes);

    /// The round's result: Stopped when cancelled; Failed on an error chunk, a refused request, a
    /// reply over kMaxResponseBytes, a transport failure or a stream that ended with neither a
    /// finish reason nor [DONE]; else Succeeded. Text keeps what arrived.
    TurnResult End(const HttpClient::HttpResponse& response);
    /// Ends the turn with one OnFinished of End().
    void Finish(const HttpClient::HttpResponse& response);
    /// The tool calls the reply asked for, in their index order.
    std::vector<ToolCall> ToolCalls() const;

private:
    void OnEvent(const SseEvent& event);
    void ReadToolCalls(const nlohmann::json& delta);
    void Fail(std::string message);

    AgentTurnEvents& m_Events;
    SseParser m_Parser;
    TurnResult m_Result;
    std::string m_FinishReason;
    std::string m_TextPrefix;
    /// The tool calls by the index the stream gives them.
    std::vector<std::pair<uint64_t, ToolCall>> m_ToolCalls;
    bool m_Done = false;
    bool m_Failed = false;
};
} // namespace GameEngine
