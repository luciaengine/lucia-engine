#include "Platform/ControllerFeedback.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <limits>
#include <stdexcept>
#include <source_location>
#include <string>

namespace
{
Uint16 strength = 0;
int SDLCALL Rumble(void*, Uint16 low, Uint16 high)
{
    strength = low > high ? low : high;
    return 0;
}
void Require(bool condition, std::source_location location = std::source_location::current())
{
    if (!condition) throw std::runtime_error("Controller feedback assertion failed at line " +
        std::to_string(location.line()) + ": " + SDL_GetError());
}
}

int main()
{
    SDL_SetMainReady();
    Require(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0);
    SDL_VirtualJoystickDesc description{};
    description.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    description.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    description.naxes = SDL_CONTROLLER_AXIS_MAX;
    description.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    description.name = "Feedback test controller";
    description.Rumble = Rumble;
    const int index = SDL_JoystickAttachVirtualEx(&description);
    Require(index >= 0);
    // Select the virtual pad explicitly; real connected pads must neither make
    // this test ambiguous nor receive its feedback pulses.
    SDL_Joystick* joystick = SDL_JoystickOpen(index);
    Require(joystick != nullptr);
    Require(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1) == 0);
    {
        GameEngine::Platform::ControllerFeedback feedback;
        feedback.Update(.18f, true);
        Require(strength > 0 && strength < 15000);
        SDL_Delay(80);
        feedback.Update(0, true);
        Require(strength == 0);
        feedback.Update(.18f, false);
        Require(strength == 0);
        feedback.Update(.18f, true);
        feedback.Update(0, true);
        Require(strength == 0);
        feedback.Update(std::numeric_limits<float>::quiet_NaN(), true);
        Require(strength == 0);
        feedback.Update(2.f, true);
        Require(strength == 65535);
        feedback.Stop();
        Require(strength == 0);
        feedback.Update(.18f, true);
    }
    Require(strength == 0);
    SDL_JoystickClose(joystick);
    Require(SDL_JoystickDetachVirtual(index) == 0);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}
