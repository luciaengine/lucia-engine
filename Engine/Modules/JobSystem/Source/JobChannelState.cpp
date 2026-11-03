#include "JobChannelState.h"

#include "JobSystem/TaskEnvelopes.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <cassert>
#include <utility>

namespace JobSystem
{
namespace Detail
{

JobChannelState::JobChannelState(WorkStealingThreadPool& pool, const char* name, uint32 maxRunning)
    : m_Pool(pool), m_Name(name), m_MaxRunning(maxRunning)
{
    assert(name != nullptr && "JobChannel: a channel needs a name");
    assert(maxRunning >= 1 && "JobChannel: MaxRunning must be at least 1");
}

JobChannelState::Admission JobChannelState::Admit(UniquePtr<TaskBase>& envelope, ChannelSlot& slot)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Pool.IsShuttingDown())
    {
        return Admission::Refused;
    }
    if (m_Running < m_MaxRunning)
    {
        ++m_Running;
        slot.MarkDispatched();
        return Admission::Dispatch;
    }
    m_Queued.push_back({std::move(envelope), &slot});
    return Admission::Queued;
}

void JobChannelState::ReleaseSlot()
{
    UniquePtr<TaskBase> next;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        assert(m_Running > 0 && "JobChannel: a slot was released twice");
        --m_Running;
        // After the gate a promotion could land in a blocking FIFO the drain
        // has already emptied; the drain owns the queued jobs from then on.
        if (m_Pool.IsShuttingDown() || m_Queued.empty())
        {
            return;
        }
        QueuedJob head = std::move(m_Queued.front());
        m_Queued.pop_front();
        ++m_Running;
        head.Slot->MarkDispatched();
        next = std::move(head.Envelope);
    }
    m_Pool.DispatchToBlockingThreads(std::move(next));
}

void JobChannelState::TakeQueued(Vector<UniquePtr<TaskBase>>& out)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (QueuedJob& job : m_Queued)
    {
        out.push_back(std::move(job.Envelope));
    }
    m_Queued.clear();
}

void JobChannelState::SetBeforeDispatchHook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_BeforeDispatchHookForTests = std::move(hook);
}

void JobChannelState::RunBeforeDispatchHook()
{
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        hook = m_BeforeDispatchHookForTests;
    }
    if (hook)
    {
        hook();
    }
}

ChannelSlot::~ChannelSlot()
{
    if (m_Dispatched)
    {
        m_Channel->ReleaseSlot();
    }
}

} // namespace Detail
} // namespace JobSystem
