#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class CancelToken;

/// Finds the OpenAI-compatible model servers running on this computer, so the custom endpoint
/// offers them by name (TensorFold, Ollama, LM Studio, llama.cpp, vLLM) instead of asking for a URL.
namespace LocalServerScan
{
/// A server that answered its model list.
struct Server
{
    /// What the setup page calls it: the name its model list gives (`owned_by`), else the name
    /// of the program that uses its port by default, else "Server at <host:port>".
    std::string Name;
    /// Its API URL (".../v1").
    std::string BaseUrl;
    /// The models it lists, in its order.
    std::vector<std::string> Models;
};

/// The API URLs the scan asks, on 127.0.0.1: 8080 (TensorFold, llama.cpp), 11434 (Ollama),
/// 1234 (LM Studio) and 8000 (vLLM).
std::span<const std::string_view> DefaultCandidates();

/// The server a model list `body` from `baseUrl` describes; a body that is not an OpenAI model
/// list gives a server with no models.
Server ParseModelList(std::string_view baseUrl, std::string_view body);

/// Asks each of `candidates` for <url>/models with a short time limit and returns the servers
/// that answered with at least one model, in `candidates` order. Stops early once `cancel` is set.
std::vector<Server> Scan(std::span<const std::string_view> candidates, const CancelToken& cancel);
} // namespace LocalServerScan
} // namespace GameEngine
