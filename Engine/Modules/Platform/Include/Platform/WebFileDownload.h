#pragma once

#if defined(__EMSCRIPTEN__)
#include <string_view>

namespace GameEngine::Platform
{
// Offer bytes as a browser download. True means the browser accepted the
// request, not that the user completed a save to disk.
bool DownloadFile(std::string_view filename, std::string_view contents);
}
#endif
