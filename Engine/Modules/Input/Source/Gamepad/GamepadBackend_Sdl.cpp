#include "Input/Gamepad/GamepadBackend.h"
#include "Input/GamepadCodes.h"
#include "Input/GamepadFrame.h"
#include "Gamepad/SnapshotTripleBuffer.h"
#include "Logger/Logger.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>

namespace GameEngine::Input
{
namespace
{
constexpr auto kDiscoveryInterval = std::chrono::milliseconds(500);
constexpr std::array kAxes{SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                          SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
constexpr std::array kButtons{SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                             SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
                             SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
                             SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
                             SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                             SDL_GAMEPAD_BUTTON_DPAD_LEFT};
static_assert(kAxes.size() == kGamepadAxisCount && kButtons.size() == kGamepadButtonCount);

// The SDL window owns the subsystem and pumps events. Bounded discovery/read jobs publish snapshots;
// the frame thread takes only a published value and never waits for SDL.
class SdlGamepadBackend final : public GamepadBackend
{
  public:
    ~SdlGamepadBackend() override { Stop(); }

    GamepadRuntimeState Start() override
    {
        if ((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) == 0)
        {
            Logger::Log::Error("SDL gamepads are unavailable: initialize the mobile window before input");
            return GamepadRuntimeState::Failed;
        }
        Discover();
        Publish();
        m_NextDiscovery = std::chrono::steady_clock::now() + kDiscoveryInterval;
        return GamepadRuntimeState::Available;
    }

    void Read(GamepadFrame& frame) override
    {
        m_Frames.Acquire();
        frame = m_Frames.ReadBuffer();
    }

    bool NeedsRefresh() const override { return true; }

    void Refresh() override
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= m_NextDiscovery)
        {
            Discover();
            m_NextDiscovery = now + kDiscoveryInterval;
        }
        Publish();
    }

    void Stop() override
    {
        for (auto& device : m_Devices)
        {
            if (device.Pad)
                SDL_CloseGamepad(device.Pad);
            device = {};
        }
    }

  private:
    struct Device
    {
        SDL_JoystickID Id = 0;
        SDL_Gamepad* Pad = nullptr;
    };

    void Discover()
    {
        for (auto& device : m_Devices)
        {
            if (device.Pad && !SDL_GamepadConnected(device.Pad))
            {
                SDL_CloseGamepad(device.Pad);
                device = {};
            }
        }
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (!ids)
            return;
        for (int i = 0; i < count; ++i)
        {
            if (std::any_of(m_Devices.begin(), m_Devices.end(), [id = ids[i]](const Device& d) { return d.Id == id; }))
                continue;
            const auto empty = std::find_if(m_Devices.begin(), m_Devices.end(), [](const Device& d) { return !d.Pad; });
            if (empty == m_Devices.end())
                break;
            if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]))
                *empty = {ids[i], pad};
        }
        SDL_free(ids);
    }

    void Publish()
    {
        GamepadFrame& frame = m_Frames.WriteBuffer();
        frame = {};
        for (int i = 0; i < kMaxGamepads; ++i)
        {
            SDL_Gamepad* pad = m_Devices[i].Pad;
            if (!pad || !SDL_GamepadConnected(pad))
                continue;
            auto& slot = frame.slots[i];
            slot.connected = true;
            for (int a = 0; a < kGamepadAxisCount; ++a)
            {
                float value = std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, kAxes[a])) / 32767.0f, -1.0f, 1.0f);
                if (a == static_cast<int>(GamepadAxis::LeftY) || a == static_cast<int>(GamepadAxis::RightY))
                    value = -value;
                if (a >= static_cast<int>(GamepadAxis::LeftTrigger))
                    value = std::max(value, 0.0f);
                slot.axes[a] = value;
            }
            for (int b = 0; b < kGamepadButtonCount; ++b)
                slot.buttons[b] = SDL_GetGamepadButton(pad, kButtons[b]);
        }
        m_Frames.Publish();
    }

    std::array<Device, kMaxGamepads> m_Devices{};
    SnapshotTripleBuffer<GamepadFrame> m_Frames;
    std::chrono::steady_clock::time_point m_NextDiscovery;
};
} // namespace

std::unique_ptr<GamepadBackend> CreatePlatformGamepadBackend()
{
    return std::make_unique<SdlGamepadBackend>();
}
} // namespace GameEngine::Input
