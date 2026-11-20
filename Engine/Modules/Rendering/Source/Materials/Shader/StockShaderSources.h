#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string_view>

namespace GameEngine::Rendering
{
struct StockShaderSource
{
    std::string_view Path;
    uint64_t Hash;
};

inline bool MatchesStockShaderSource(const std::filesystem::path& relativePath, uint64_t hash)
{
    static constexpr StockShaderSource kSources[] = {
        {"", 0},
#include "StockShaderHashes.inc"
    };
    const auto path = relativePath.generic_string();
    const auto entry = std::lower_bound(std::begin(kSources), std::end(kSources), path,
        [](const StockShaderSource& source, std::string_view name) { return source.Path < name; });
    return entry != std::end(kSources) && !entry->Path.empty() && entry->Path == path && entry->Hash == hash;
}
} // namespace GameEngine::Rendering
