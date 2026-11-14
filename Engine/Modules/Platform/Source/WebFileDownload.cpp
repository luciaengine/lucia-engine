#include "Platform/WebFileDownload.h"
#include <emscripten/em_js.h>
#include <string>

EM_JS(int, GeWebDownloadFile, (const char* filename, const char* data, size_t size), {
    let url;
    let anchor;
    try {
        const bytes = HEAPU8.slice(data, data + size);
        url = URL.createObjectURL(new Blob([bytes], {type: 'application/octet-stream'}));
        anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = UTF8ToString(filename);
        document.body.appendChild(anchor);
        anchor.click();
        return 1;
    } catch (error) {
        console.error('Download failed:', error);
        return 0;
    } finally {
        if (anchor) anchor.remove();
        // Give the browser time to consume the Blob before releasing it.
        if (url) setTimeout(() => URL.revokeObjectURL(url), 60000);
    }
});

namespace GameEngine::Platform
{
bool DownloadFile(std::string_view filename, std::string_view contents)
{
    // A download name is a leaf, never a path into the host filesystem.
    const auto separator = filename.find_last_of("/\\");
    if (separator != std::string_view::npos) filename.remove_prefix(separator + 1);
    if (filename.empty() || filename == "." || filename == ".." || filename.find('\0') != std::string_view::npos)
        return false;
    const std::string name(filename);
    return GeWebDownloadFile(name.c_str(), contents.data(), contents.size()) != 0;
}
}
