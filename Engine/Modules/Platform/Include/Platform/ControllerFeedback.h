#pragma once
#include <memory>

namespace GameEngine::Platform
{
/** Single-player feedback routed to the most recently used desktop controller.
 * Call Update on the main thread; destruction stops motors and releases devices.
 */
class ControllerFeedback
{
public:
    ControllerFeedback();
    ~ControllerFeedback();
    ControllerFeedback(const ControllerFeedback&) = delete;
    ControllerFeedback& operator=(const ControllerFeedback&) = delete;
    void Update(float intensity, bool focused);
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
}
