#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace GameEngine::PolyhavenThumbnail
{

inline std::string PngUrl(std::string_view url)
{
    const auto fragment = url.find('#');
    const auto resource = url.substr(0, fragment);
    const auto query = resource.find('?');
    std::string result(resource.substr(0, query));
    bool hasQuery = false;
    if (query != std::string_view::npos)
    {
        auto remaining = resource.substr(query + 1);
        while (!remaining.empty())
        {
            const auto end = remaining.find('&');
            const auto parameter = remaining.substr(0, end);
            if (!parameter.empty() && parameter.substr(0, parameter.find('=')) != "format")
            {
                result += hasQuery ? '&' : '?';
                result += parameter;
                hasQuery = true;
            }
            if (end == std::string_view::npos)
                break;
            remaining.remove_prefix(end + 1);
        }
    }
    result += hasQuery ? "&format=png" : "?format=png";
    if (fragment != std::string_view::npos)
        result += url.substr(fragment);
    return result;
}

inline bool HasPngSignature(std::string_view bytes)
{
    return bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
}

inline bool HasCachedPngHeader(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    char signature[8]{};
    input.read(signature, sizeof(signature));
    return input.gcount() == static_cast<std::streamsize>(sizeof(signature)) &&
           HasPngSignature(std::string_view(signature, sizeof(signature)));
}

} // namespace GameEngine::PolyhavenThumbnail
