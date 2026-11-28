#include "Panels/LightTablePanel.h"

#include "Platform/HttpClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

namespace GameEngine::Editor
{
namespace
{
using nlohmann::json;
using namespace std::chrono_literals;

struct Instance
{
    std::string Origin;
    std::string Token;
    std::string Catalog;
    int64_t ProcessId;
};

std::optional<Instance> ReadInstance()
{
    const char* path = std::getenv("LUCIA_LIGHTTABLE_INSTANCE_FILE");
    if (!path || !*path)
        return std::nullopt;
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    std::array<char, 64 * 1024> bytes{};
    file.read(bytes.data(), bytes.size());
    if (!file.eof() || file.bad())
        return std::nullopt;
    const auto data = json::parse(bytes.data(), bytes.data() + file.gcount(), nullptr, false);
    if (!data.is_object() || !data.contains("port") || !data["port"].is_number_integer() ||
        !data.contains("pid") || !data["pid"].is_number_integer() ||
        !data.contains("token") || !data["token"].is_string() ||
        !data.contains("catalog") || !data["catalog"].is_string() || data["catalog"] == "")
        return std::nullopt;
    const auto port = data["port"].get<int64_t>();
    const auto process = data["pid"].get<int64_t>();
    const auto& token = data["token"].get_ref<const std::string&>();
    if (port < 1 || port > 65535 || process < 1 || token.size() < 16 || token.size() > 256 ||
        !std::ranges::all_of(token, [](char c) {
            return std::string_view("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_").find(c)
                   != std::string_view::npos;
        }))
        return std::nullopt;
    return Instance{"http://127.0.0.1:" + std::to_string(port), token, data["catalog"], process};
}

std::string StartPage()
{
    if (const auto instance = ReadInstance())
        return instance->Origin + "/";
    return "http://127.0.0.1:8321/";
}

json Error(std::string message)
{
    return {{"error", std::move(message)}};
}

json Response(const HttpClient::HttpResponse& response)
{
    if (!response.success)
        return nullptr;
    auto data = json::parse(response.body, nullptr, false);
    return data.is_object() ? std::move(data) : json(nullptr);
}
}

LightTablePanel::LightTablePanel() : WebPanel("LightTable", StartPage()) {}

std::string LightTablePanel::ConnectedOrigin() const
{
    const auto instance = ReadInstance();
    if (!instance || CurrentPageURL() != instance->Origin + "/")
        return {};
    return instance->Origin;
}

nlohmann::json LightTablePanel::RunDocumentCommand(const std::string& origin, const json& request)
{
    const auto instance = ReadInstance();
    if (!instance || instance->Origin != origin)
        return Error("Open the configured local LightTable catalog in the LightTable panel first.");

    constexpr size_t responseLimit = 2 * 1024 * 1024;
    const auto health = Response(HttpClient::Get(origin + "/api/health", {},
        {.MaxResponseBytes = responseLimit, .Timeout = 3s}));
    if (!health.is_object() || !health.contains("pid") || health["pid"] != instance->ProcessId ||
        !health.contains("catalog") || health["catalog"] != instance->Catalog)
        return Error("The configured LightTable catalog changed. Reconnect the panel first.");

    const std::string documentId = origin + "/" + std::to_string(instance->ProcessId) + "/" +
        std::to_string(std::hash<std::string>{}(instance->Catalog));
    const auto port = origin.substr(origin.rfind(':') + 1);
    const std::unordered_map<std::string, std::string> headers{
        {"Cookie", "lighttable_token_" + port + "=" + instance->Token},
        {"X-LightTable-Strict", "1"},
    };
    const auto post = [&](std::string_view route, const json& body) {
        return Response(HttpClient::PostJson(origin + std::string(route), headers, body.dump(),
            {.Timeout = 3s, .MaxResponseBytes = responseLimit}));
    };

    if (request["operation"] == "inspect")
    {
        const auto offset = request.find("offset");
        const auto page = post("/api/catalog/query", {{"offset", offset != request.end() && offset->is_number_integer() ? *offset : json(0)},
                                                      {"limit", 100}});
        if (!page.is_object() || !page.contains("items") || !page["items"].is_array())
            return Error("LightTable could not read the catalog.");
        json photos = json::array();
        for (const auto& item : page["items"])
        {
            if (item.contains("name") && item["name"].is_string())
                photos.push_back({{"id", item["name"]}, {"name", item.value("displayName", item["name"])},
                                  {"rating", item.value("rating", json(0))}});
        }
        return {{"documentId", documentId}, {"photos", std::move(photos)}, {"total", page.value("total", json(0))}};
    }

    const auto photo = request.find("itemId");
    const auto rating = request.find("rating");
    if (!request.contains("documentId") || request["documentId"] != documentId)
        return Error("The LightTable catalog changed. Inspect it again.");
    if (request["operation"] != "rate_photo" || photo == request.end() || !photo->is_string() ||
        photo->get_ref<const std::string&>().empty() || photo->get_ref<const std::string&>().size() > 4096 ||
        rating == request.end() || !rating->is_number_integer() || *rating < 0 || *rating > 5)
        return Error("Provide a LightTable photo id and a rating from zero to five.");
    const auto result = post("/api/state", {{"name", *photo}, {"rating", *rating},
                                            {"origin", "agent:lucia"}, {"historyLabel", "Rate photo"}});
    if (!result.is_object() || !result.value("ok", false))
        return Error("LightTable did not accept the rating.");
    return {{"documentId", documentId}, {"itemId", *photo}, {"rating", *rating}};
}
}
