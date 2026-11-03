#pragma once

#include "AssetCore/Types.h"

#include <filesystem>
#include <string>
#include <unordered_set>

namespace GameEngine
{
namespace TileMap
{
namespace Importers
{

/**
 * A path as UTF-8 text, for names and messages. path::string() converts
 * through the Windows code page and throws on a character outside it.
 */
inline std::string PathText(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/** Reduces a foreign identifier to a safe asset file stem (never empty). */
inline std::string SanitizeAssetName(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ' || c == '(' ||
                        c == ')';
        out.push_back(ok ? c : '_');
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? std::string("Imported") : out;
}

/**
 * Resolves a source-relative path against the source file's directory.
 *
 * A foreign file may only reference paths under its own directory: an absolute
 * path, and any `..` chain that climbs out, resolve to an empty path. Importing
 * hands these to the caller's texture resolver — which in the editor registers
 * them as project assets — so containment is what keeps a hostile .tmx from
 * naming arbitrary files on the machine. Callers must treat an empty result as
 * a rejection, not as "no path given".
 */
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& sourceFile,
                                               const std::string& relative)
{
    if (relative.empty() || relative.find('\0') != std::string::npos)
        return {};
    const std::filesystem::path rel(relative);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory())
        return {};

    std::error_code error;
    const auto directory = sourceFile.parent_path().empty() ? std::filesystem::path(".") : sourceFile.parent_path();
    const std::filesystem::path base = std::filesystem::weakly_canonical(directory, error);
    if (error)
        return {};
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(base / rel, error);
    if (error)
        return {};
    // Resolve existing symlinks before comparing, including the parent of a missing image.
    const std::filesystem::path inside = resolved.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..")
        return {};
    return resolved;
}

/** Reserves one output filename under case-insensitive filesystem semantics. */
inline bool ReserveImportName(std::unordered_set<std::string>& names, std::string name,
                              const char* extension)
{
    name += extension;
    for (char& c : name)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return names.insert(std::move(name)).second;
}

/** Parses "#RRGGBB" / "RRGGBB" into packed 0xRRGGBBAA (alpha FF); white on failure. */
inline uint32 ParseHexColorRGBA(const std::string& text)
{
    const size_t start = (!text.empty() && text[0] == '#') ? 1 : 0;
    if (text.size() - start != 6)
        return 0xFFFFFFFFu;
    uint32 rgb = 0;
    for (size_t i = start; i < text.size(); ++i)
    {
        const char c = text[i];
        uint32 digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<uint32>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<uint32>(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<uint32>(c - 'A') + 10u;
        else
            return 0xFFFFFFFFu;
        rgb = (rgb << 4) | digit;
    }
    return (rgb << 8) | 0xFFu;
}

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
