#include "Platform/ControllerFeedback.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace GameEngine::Platform
{
struct ControllerFeedback::Impl
{
    struct Device
    {
        SDL_GameController* controller = nullptr;
        SDL_JoystickID id = -1;
        std::array<Uint8, SDL_CONTROLLER_BUTTON_MAX> buttons{};
        std::array<Sint16, SDL_CONTROLLER_AXIS_MAX> axes{};
        Uint16 sent = 0;
    };
    bool initialized = false;
    bool attempted = false;
    Uint32 nextScan = 0;
    SDL_JoystickID active = -1;
    std::vector<Device> devices;

    void Scan()
    {
        for (auto it = devices.begin(); it != devices.end();)
        {
            if (!SDL_GameControllerGetAttached(it->controller))
            {
                if (active == it->id) active = -1;
                SDL_GameControllerClose(it->controller);
                it = devices.erase(it);
            }
            else ++it;
        }
        for (int index = 0; index < SDL_NumJoysticks(); ++index)
        {
            const auto id = SDL_JoystickGetDeviceInstanceID(index);
            if (!SDL_IsGameController(index) ||
                std::any_of(devices.begin(), devices.end(), [id](const Device& d) { return d.id == id; })) continue;
            if (auto* controller = SDL_GameControllerOpen(index)) devices.push_back({controller, id, {}, {}});
        }
    }
};

ControllerFeedback::ControllerFeedback() : m_Impl(std::make_unique<Impl>()) {}
ControllerFeedback::~ControllerFeedback()
{
    Stop();
    for (auto& device : m_Impl->devices) SDL_GameControllerClose(device.controller);
    if (m_Impl->initialized) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void ControllerFeedback::Stop()
{
    for (auto& device : m_Impl->devices)
    {
        SDL_GameControllerRumble(device.controller, 0, 0, 0);
        device.sent = 0;
    }
}

void ControllerFeedback::Update(float intensity, bool focused)
{
    auto& state = *m_Impl;
    if (!state.attempted)
    {
        if (!focused) return;
        state.attempted = true;
        // GLFW owns the window; focus is supplied by the caller, not SDL video.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetMainReady();
        state.initialized = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
        if (!state.initialized) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Controller feedback unavailable: %s", SDL_GetError());
    }
    if (!state.initialized) return;
    SDL_GameControllerUpdate();
    const auto now = SDL_GetTicks();
    if (SDL_TICKS_PASSED(now, state.nextScan))
    {
        state.Scan();
        state.nextScan = now + 1000;
    }
    for (auto& device : state.devices)
    {
        bool pressed = false;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
        {
            const auto down = SDL_GameControllerGetButton(device.controller, static_cast<SDL_GameControllerButton>(button));
            pressed |= down && !device.buttons[button];
            device.buttons[button] = down;
        }
        for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis)
        {
            const auto value = SDL_GameControllerGetAxis(device.controller, static_cast<SDL_GameControllerAxis>(axis));
            // Dead zone and movement threshold avoid selecting an idle drifting stick.
            pressed |= std::abs(int(value)) > 12000 && std::abs(int(value) - int(device.axes[axis])) > 4000;
            device.axes[axis] = value;
        }
        if (focused && pressed) state.active = device.id;
    }
    if (state.active < 0 && state.devices.size() == 1) state.active = state.devices.front().id;
    const auto strength = static_cast<Uint16>((focused && std::isfinite(intensity) ? std::clamp(intensity, 0.f, 1.f) : 0.f) * 65535.f);
    for (auto& device : state.devices)
    {
        const auto value = device.id == state.active ? strength : Uint16(0);
        // Hardware timeout protects against stalls and process termination.
        if (value || device.sent)
            SDL_GameControllerRumble(device.controller, value, value, value ? 50 : 0);
        device.sent = value;
    }
}
}
