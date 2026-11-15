#include "FileSystem/FileSystem.h"

namespace GameEngine::FileSystem
{

std::filesystem::path ProjectLibraryRoot()
{
    return {};
}

bool IsCaseSensitive()
{
#if defined(__linux__) || defined(GE_PLATFORM_IOS)
    return true;
#else
    return false;
#endif
}

} // namespace GameEngine::FileSystem
