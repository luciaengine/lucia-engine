#include "Input/GamepadPolling.h"
#include "Input/GamepadFrame.h"
#include "Platform/Gamepad.h"
namespace GameEngine::Input
{
void ReadGamepadFrame(GamepadFrame& frame)
{
    frame = {};
    for (int i = 0; i < kMaxGamepads; ++i)
    {
        float axes[Platform::kMappedGamepadAxisCount]{};
        unsigned char buttons[Platform::kMappedGamepadButtonCount]{};
        int axisCount = 0, buttonCount = 0;
        if (!Platform::ReadMappedGamepadState(i, axes, buttons, axisCount, buttonCount)) continue;
        auto& slot = frame.slots[i];
        slot.connected = true;
        for (int a = 0; a < axisCount; ++a) slot.axes[a] = axes[a];
        for (int b = 0; b < buttonCount; ++b) slot.buttons[b] = buttons[b] != 0;
    }
}
}
