#pragma once

#include "IAgentProvider.h"
#include "SseParser.h"

#include "Platform/HttpClient.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
class CancelToken;

/// One OpenAI-compatible chat completions turn as it streams (POST .../chat/completions with
/// stream true): each server-sent event's data is a chunk whose `choices[0].delta.content` text
/// becomes OnTextDelta in arrival order; `choices[0].finish_reason` ends the reply (a "length" or
/// "content_filter" reason becomes the turn's Note); `usage` carries the token counts; `model`
/// names the model that answered; `data: [DONE]` ends the stream. A chunk with an `error` object
/// fails the turn with its message; a chunk that is not JSON fails it too.
class OpenAiChatStream
{
public:
    /// The response body cap: the same size as the Claude API stream's.
    static constexpr size_t kMaxResponseBytes = size_t{16384} * 512;

    /// Posts `body` to `url` with `headers`, streams the reply into `events` and ends with
    /// exactly one OnFinished. `cancel` stops the transfer.
    static void Run(const std::string& url,
                    const std::unordered_map<std::string, std::string>& headers,
                    const std::string& body,
                    AgentTurnEvents& events,
                    const CancelToken& cancel);

    explicit OpenAiChatStream(AgentTurnEvents& events);

    /// Consumes the next response bytes, split anywhere.
    void Feed(std::string_view bytes);

    /// Ends the turn with one OnFinished: Stopped when cancelled; Failed on an error chunk, a
    /// refused request, a reply over kMaxResponseBytes, a transport failure or a stream that
    /// ended with neither a finish reason nor [DONE]; else Succeeded. Text keeps what arrived.
    void Finish(const HttpClient::HttpResponse& response);

private:
    void OnEvent(const SseEvent& event);
    void Fail(std::string message);

    AgentTurnEvents& m_Events;
    SseParser m_Parser;
    TurnResult m_Result;
    std::string m_FinishReason;
    bool m_Done = false;
    bool m_Failed = false;
};
} // namespace GameEngine
