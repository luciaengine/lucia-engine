#include "Engine/Build/PythonBuildTool.h"
#include <cstdlib>
#include <sstream>
#include <vector>

namespace GameEngine
{
std::filesystem::path FindPythonBuildExecutable()
{
#if defined(_WIN32)
    const char* names[] = {"python.exe", "python3.exe"};
    const char kPathSeparator = ';';
#else
    const char* names[] = {"python3", "python"};
    const char kPathSeparator = ':';
#endif

    std::vector<std::filesystem::path> searchDirs;
#if defined(__APPLE__)
    // A bundled Editor launched from Finder inherits a minimal PATH, so the
    // usual package-manager prefixes are searched explicitly.
    searchDirs.emplace_back("/opt/homebrew/bin");
    searchDirs.emplace_back("/usr/local/bin");
    searchDirs.emplace_back("/usr/bin");
#endif
    if (const char* pathEnv = std::getenv("PATH"))
    {
        std::stringstream stream(pathEnv);
        std::string entry;
        while (std::getline(stream, entry, kPathSeparator))
        {
            if (!entry.empty())
                searchDirs.emplace_back(entry);
        }
    }

    std::error_code ec;
    for (const char* name : names)
    {
        for (const auto& dir : searchDirs)
        {
            const auto candidate = dir / name;
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        }
    }
    return {};
}

}
