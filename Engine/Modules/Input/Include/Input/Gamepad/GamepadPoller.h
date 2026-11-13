#pragma once

#include "JobSystem/TaskHandle.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace JobSystem
{
class JobChannel;
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::Input
{

class GamepadBackend;
struct GamepadFrame;
enum class GamepadRuntimeState : std::uint8_t;

/**
 * @brief Reads every gamepad once per frame through the platform's GamepadBackend.
 * The backend's Start and optional bounded Refresh run on blocking jobs. An inline pool runs
 * those jobs synchronously on the caller. Until Start returns
 * Available, Read reports no pad and does not wait; a pad present at start arrives as a hot plug.
 */
class GamepadPoller
{
  public:
    /// Construct on the thread that calls Read. The pool must outlive the poller.
    GamepadPoller(JobSystem::WorkStealingThreadPool& pool, std::unique_ptr<GamepadBackend> backend);
    /// Joins Start and the last bounded refresh, then stops the backend before the pool shuts down.
    ~GamepadPoller();
    GamepadPoller(const GamepadPoller&) = delete;
    GamepadPoller& operator=(const GamepadPoller&) = delete;

    /// Fills `frame` with this frame's state. Only on the constructing thread.
    void Read(GamepadFrame& frame);

    /// The state of this process's gamepad runtime: Starting until a poller's Start returns, then
    /// that outcome. The outcome stays after the poller stops.
    static GamepadRuntimeState GetRuntimeState();

  private:
    void RunStart(bool inlinePool);

    std::unique_ptr<GamepadBackend> m_Backend;
    std::unique_ptr<JobSystem::JobChannel> m_StartChannel;
    JobSystem::TaskHandle m_Start;
    JobSystem::TaskHandle m_Refresh;
    std::thread::id m_FrameThread;
    // Release-stored by the start job when Start returns Available; Read acquires it.
    std::atomic<bool> m_Ready{false};
};

} // namespace GameEngine::Input
