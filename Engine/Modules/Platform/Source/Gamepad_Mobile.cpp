#include "Platform/Gamepad.h"
#include <SDL3/SDL.h>
namespace GameEngine::Platform
{
bool ReadMappedGamepadState(int slot, float* axes, unsigned char* buttons, int& axisCount, int& buttonCount)
{
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    if (!ids || slot < 0 || slot >= count) { SDL_free(ids); return false; }
    auto* pad = SDL_GetGamepadFromID(ids[slot]);
    if (!pad) pad = SDL_OpenGamepad(ids[slot]);
    SDL_free(ids);
    if (!pad) return false;
    constexpr SDL_GamepadAxis axisMap[] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
    constexpr SDL_GamepadButton buttonMap[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT};
    for (int i = 0; i < kMappedGamepadAxisCount; ++i) axes[i] = SDL_GetGamepadAxis(pad, axisMap[i]) / 32767.0f;
    for (int i = 0; i < kMappedGamepadButtonCount; ++i) buttons[i] = SDL_GetGamepadButton(pad, buttonMap[i]);
    axisCount = kMappedGamepadAxisCount; buttonCount = kMappedGamepadButtonCount;
    return true;
}
}
