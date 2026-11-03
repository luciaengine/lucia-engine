#include "Platform/Clipboard.h"
#include <SDL3/SDL.h>
namespace GameEngine::Platform
{
std::string GetClipboardText()
{
    char* text = SDL_GetClipboardText();
    std::string result = text ? text : "";
    SDL_free(text);
    return result;
}
void SetClipboardText(const char* text) { SDL_SetClipboardText(text ? text : ""); }
}
