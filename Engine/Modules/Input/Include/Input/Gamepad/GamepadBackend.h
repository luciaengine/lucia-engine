#pragma once

#include <cstdint>
#include <memory>

namespace GameEngine::Input
{

struct GamepadFrame;
enum class GamepadRuntimeState : std::uint8_t;

/**
 * @brief One platform's gamepad source, driven by GamepadPoller: Start once on a blocking job, Read
 * on the frame thread after Start returned Available, optional bounded Refresh jobs, and Stop
 * once after Start, the last Refresh and the last Read.
 * Device discovery belongs to Start, Refresh and platform callbacks: Read never enumerates.
 */
class GamepadBackend
{
  public:
    virtual ~GamepadBackend() = default;

    /// Creates the platform API and starts device discovery. Runs on a blocking job, never on
    /// the main thread with a worker pool; an inline pool runs it synchronously.
    /// @return Available when gamepads can be read, otherwise the reason.
    virtual GamepadRuntimeState Start() = 0;

    /// Fills `frame` from the latest device snapshot. Never enumerates devices and never waits.
    virtual void Read(GamepadFrame& frame) = 0;

    /// Polling backends request one bounded refresh job per frame. Callback-driven backends do not.
    virtual bool NeedsRefresh() const { return false; }
    /// Runs on the poller's blocking channel, never concurrently with Start, another Refresh or Stop.
    virtual void Refresh() {}

    /// Stops discovery and releases devices after the last Refresh and Read. Also valid before Start.
    virtual void Stop() = 0;
};

/// GameInput on Windows, Game Controller on Apple platforms, SDL3 on other mobile platforms,
/// and GLFW's joystick API on desktop Linux and in the browser.
std::unique_ptr<GamepadBackend> CreatePlatformGamepadBackend();

} // namespace GameEngine::Input
