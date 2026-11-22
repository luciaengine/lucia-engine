#include "BackendDetection.h"

#include <cstdlib>
#include <system_error>

namespace GameEngine::BackendDetection
{
namespace
{
#if defined(_WIN32)
constexpr char kPathSeparator = ';';
constexpr std::string_view kProgramSuffix = ".exe";
#else
constexpr char kPathSeparator = ':';
constexpr std::string_view kProgramSuffix = "";
#endif

std::string Environment(const char* name)
{
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

bool IsProgram(const std::filesystem::path& candidate)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error))
        return false;
#if defined(_WIN32)
    return true;
#else
    const std::filesystem::perms perms = std::filesystem::status(candidate, error).permissions();
    return !error && (perms & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                               std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
#endif
}

std::optional<std::filesystem::path> FindIn(const std::filesystem::path& folder, std::string_view program)
{
    if (folder.empty())
        return std::nullopt;
    std::filesystem::path candidate = folder / (std::string(program) + std::string(kProgramSuffix));
    if (IsProgram(candidate))
        return candidate;
    return std::nullopt;
}
} // namespace

Inputs CurrentInputs()
{
#if defined(_WIN32)
    return {.Path = Environment("PATH"), .Home = Environment("USERPROFILE")};
#else
    return {.Path = Environment("PATH"), .Home = Environment("HOME")};
#endif
}

std::vector<std::filesystem::path> InstallFolders(std::string_view program, const Inputs& inputs)
{
    std::vector<std::filesystem::path> folders;
    if (!inputs.Home.empty())
        folders.push_back(inputs.Home / ".local" / "bin");
#if !defined(_WIN32)
    if (program == "claude" && !inputs.Home.empty())
        folders.push_back(inputs.Home / ".claude" / "local");
    folders.emplace_back("/opt/homebrew/bin");
    folders.emplace_back("/usr/local/bin");
    if (!inputs.Home.empty())
    {
        folders.push_back(inputs.Home / ".npm-global" / "bin");
        folders.push_back(inputs.Home / ".bun" / "bin");
    }
#endif
    return folders;
}

std::optional<Found> FindProgram(std::string_view program, const Inputs& inputs)
{
    size_t start = 0;
    while (start <= inputs.Path.size())
    {
        const size_t end = inputs.Path.find(kPathSeparator, start);
        const std::string folder =
            inputs.Path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (const std::optional<std::filesystem::path> found = FindIn(folder, program))
            return Found{.Program = *found, .OnPath = true};
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    for (const std::filesystem::path& folder : InstallFolders(program, inputs))
        if (const std::optional<std::filesystem::path> found = FindIn(folder, program))
            return Found{.Program = *found, .OnPath = false};
    return std::nullopt;
}
} // namespace GameEngine::BackendDetection
