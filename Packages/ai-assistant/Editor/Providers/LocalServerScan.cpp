#include "LocalServerScan.h"

#include "CancelToken.h"
#include "JsonFields.h"

#include "Platform/HttpClient.h"

#include <array>
#include <chrono>

#include <nlohmann/json.hpp>

namespace GameEngine::LocalServerScan
{
namespace
{
constexpr std::array<std::string_view, 4> kCandidates = {
    "http://127.0.0.1:8080/v1",
    "http://127.0.0.1:11434/v1",
    "http://127.0.0.1:1234/v1",
    "http://127.0.0.1:8000/v1",
};
// A server on this computer answers at once; one that does not is not running.
constexpr std::chrono::milliseconds kScanTimeout = std::chrono::milliseconds(800);
constexpr size_t kMaxModelsResponseBytes = size_t{1024} * 1024;

// The display name of a model list's `owned_by` value, for the servers that set one.
std::string_view OwnerName(std::string_view owner)
{
    if (owner == "tensorfold")
        return "TensorFold";
    if (owner == "llamacpp")
        return "llama.cpp";
    if (owner == "vllm")
        return "vLLM";
    if (owner == "ollama" || owner == "library")
        return "Ollama";
    return {};
}

// "127.0.0.1:8080" from "http://127.0.0.1:8080/v1".
std::string_view HostAndPort(std::string_view baseUrl)
{
    if (const size_t scheme = baseUrl.find("://"); scheme != std::string_view::npos)
        baseUrl.remove_prefix(scheme + 3);
    if (const size_t slash = baseUrl.find('/'); slash != std::string_view::npos)
        baseUrl = baseUrl.substr(0, slash);
    return baseUrl;
}

// The program that uses the port by default; 8080 is shared, so it has none.
std::string_view PortName(std::string_view hostAndPort)
{
    if (hostAndPort.ends_with(":11434"))
        return "Ollama";
    if (hostAndPort.ends_with(":1234"))
        return "LM Studio";
    if (hostAndPort.ends_with(":8000"))
        return "vLLM";
    return {};
}
} // namespace

std::span<const std::string_view> DefaultCandidates()
{
    return kCandidates;
}

Server ParseModelList(std::string_view baseUrl, std::string_view body)
{
    Server server{.BaseUrl = std::string(baseUrl)};
    std::string_view owner;
    const nlohmann::json payload = nlohmann::json::parse(body, nullptr, false);
    if (payload.is_object())
    {
        const auto data = payload.find("data");
        if (data != payload.end() && data->is_array())
            for (const nlohmann::json& entry : *data)
            {
                if (!entry.is_object())
                    continue;
                if (const std::string id = JsonFields::String(entry, "id"); !id.empty())
                    server.Models.push_back(id);
                if (owner.empty())
                    owner = OwnerName(JsonFields::String(entry, "owned_by"));
            }
    }
    const std::string_view hostAndPort = HostAndPort(baseUrl);
    if (!owner.empty())
        server.Name = std::string(owner);
    else if (const std::string_view portName = PortName(hostAndPort); !portName.empty())
        server.Name = std::string(portName);
    else
        server.Name = "Server at " + std::string(hostAndPort);
    return server;
}

std::vector<Server> Scan(std::span<const std::string_view> candidates, const CancelToken& cancel)
{
    std::vector<Server> servers;
    for (const std::string_view candidate : candidates)
    {
        if (cancel.IsCancelled())
            break;
        const HttpClient::HttpResponse response =
            HttpClient::Get(std::string(candidate) + "/models", {},
                            {.MaxResponseBytes = kMaxModelsResponseBytes, .Timeout = kScanTimeout,
                             .Cancellation = &cancel.Flag()});
        if (!response.success)
            continue;
        Server server = ParseModelList(candidate, response.body);
        if (!server.Models.empty())
            servers.push_back(std::move(server));
    }
    return servers;
}
} // namespace GameEngine::LocalServerScan
