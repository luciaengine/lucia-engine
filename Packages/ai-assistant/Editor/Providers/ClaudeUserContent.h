#pragma once

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
struct AgentTurnRequest;

/// The `content` of the new user message in Claude's message format, shared by the Messages API
/// and the Claude Code session's stream-json input: the text alone, or with images, one base64
/// image block each before the text block (the order Anthropic documents for questions about
/// images).
nlohmann::json ClaudeUserContent(const AgentTurnRequest& request);
} // namespace GameEngine
