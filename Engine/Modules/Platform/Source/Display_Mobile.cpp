#include "Platform/Display.h"
#include <SDL3/SDL.h>
namespace GameEngine::Platform
{
MonitorInfo GetPrimaryMonitorInfo()
{
    MonitorInfo info;
    const auto id = SDL_GetPrimaryDisplay();
    const auto* mode = SDL_GetCurrentDisplayMode(id);
    info.name = SDL_GetDisplayName(id);
    if (mode) { info.width = info.workWidth = mode->w; info.height = info.workHeight = mode->h; info.refreshRate = mode->refresh_rate; }
    return info;
}
std::vector<MonitorInfo> EnumerateMonitors() { return {GetPrimaryMonitorInfo()}; }
void GetPrimaryMonitorWorkArea(int& x, int& y, int& w, int& h) { auto info = GetPrimaryMonitorInfo(); x = y = 0; w = info.width; h = info.height; }
int GetActiveMonitorIndexForWindow(GLFWwindow*) { return 0; }
MonitorInfo GetMonitorInfoForWindow(GLFWwindow*) { return GetPrimaryMonitorInfo(); }
bool GetActiveDisplayLiveEdr(GLFWwindow*, float&, float&) { return false; }
bool IsGamescopeSession() { return false; }
bool IsSteamDeckSession() { return false; }
bool LooksLikeLGOledC7(const std::string&) { return false; }
}
