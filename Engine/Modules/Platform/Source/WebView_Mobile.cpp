#include "Platform/WebView.h"
#include "Logger/Logger.h"
namespace GameEngine::Platform
{
std::unique_ptr<IWebView> CreateWebView(Window*, const std::filesystem::path&)
{
    Logger::Log::Error("Embedded browser views are unavailable in the mobile Player.");
    return nullptr;
}
}
