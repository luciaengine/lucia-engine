#include "Platform/Shell.h"
#include "Logger/Logger.h"
#include <SDL3/SDL.h>
namespace GameEngine::Platform
{
bool OpenUrl(const std::string& url) { return SDL_OpenURL(url.c_str()); }
bool OpenPath(const std::filesystem::path&) { return false; }
bool ShowInFileManager(const std::filesystem::path&) { return false; }
bool LaunchDetached(const std::filesystem::path&, const std::vector<std::string>&, const std::filesystem::path&) { return false; }
int LaunchDetachedGetPid(const std::filesystem::path&, const std::vector<std::string>&, const std::filesystem::path&) { return 0; }
bool IsProcessRunning(int) { return false; }
bool OpenScriptWithProject(const std::filesystem::path&, const std::filesystem::path&) { return false; }
bool OpenSourceWithProject(const std::filesystem::path&, const std::filesystem::path&) { return false; }
bool SupportsFolderPicker() { return false; }
std::filesystem::path SelectFolder(const std::filesystem::path&) { return {}; }
std::filesystem::path SelectFile(const std::filesystem::path&, const char*, const char*) { return {}; }
std::vector<std::filesystem::path> SelectFiles(const std::filesystem::path&, const char*, const char*) { return {}; }
std::filesystem::path SaveFile(const std::filesystem::path&, const char*, const char*) { return {}; }
int ShowNativeChoiceDialog(const std::string& title, const std::string& message, const std::vector<std::string>&, int defaultButton)
{
    Logger::Log::Error("{}: {}", title, message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), message.c_str(), nullptr);
    return defaultButton;
}
std::filesystem::path GetExecutablePath() { return std::filesystem::path(SDL_GetBasePath()) / "Player"; }
bool MoveToTrash(const std::filesystem::path&) { return false; }
void ReleaseTransientFiles(const std::vector<std::filesystem::path>&) {}
}
