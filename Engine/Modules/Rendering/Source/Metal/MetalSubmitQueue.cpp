#include "MetalSubmitQueue.h"
#include "MetalDevice.h"

#include "Logger/Logger.h"

namespace GameEngine
{
namespace Rendering
{

namespace
{
constexpr uint64_t kWaitSliceMs = 1000;
constexpr int kWaitWarnAfterSlices = 5;
} // namespace

MetalSubmitQueue::~MetalSubmitQueue()
{
    Stop();
}

void MetalSubmitQueue::Start(MTL::CommandQueue* queue, MetalDevice& device)
{
    m_Queue = queue;
    m_Device = &device;
    m_ShutdownRequested = false;
    m_Thread = std::thread([this]() { Run(); });
}

void MetalSubmitQueue::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Thread.joinable())
        {
            return;
        }
        m_ShutdownRequested = true;
    }
    m_WorkCv.notify_all();
    m_Thread.join();
}

void MetalSubmitQueue::Enqueue(Submit&& submit)
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        // Inline fast path: nothing pending and nothing to wait on — commit
        // synchronously (under the lock so a concurrent Enqueue can't reorder).
        if (m_Pending.empty() && !m_Busy && submit.Waits.empty())
        {
            Execute(submit);
            return;
        }
        m_Pending.push_back(std::move(submit));
    }
    m_WorkCv.notify_one();
}

void MetalSubmitQueue::Flush()
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_IdleCv.wait(lock, [this]() { return m_Pending.empty() && !m_Busy; });
}

void MetalSubmitQueue::Run()
{
    for (;;)
    {
        Submit submit;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_WorkCv.wait(lock, [this]() { return m_ShutdownRequested || !m_Pending.empty(); });
            if (m_Pending.empty())
            {
                return;
            }
            submit = std::move(m_Pending.front());
            m_Pending.pop_front();
            m_Busy = true;
        }
        Execute(submit);
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Busy = false;
        }
        m_IdleCv.notify_all();
    }
}

void MetalSubmitQueue::Execute(Submit& submit)
{
    const NS::SharedPtr<NS::AutoreleasePool> autoreleasePool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    for (auto& [event, value] : submit.Waits)
    {
        int slices = 0;
        for (;;)
        {
            if (m_ShutdownRequested || m_Device->GetDeviceHealth() != DeviceHealth::Healthy)
            {
                Cancel(submit);
                return;
            }
            if (event->waitUntilSignaledValue(value, kWaitSliceMs))
                break;
            if (++slices == kWaitWarnAfterSlices)
            {
                Logger::Log::Warning("MetalSubmitQueue: still waiting on event value {} after {}s",
                                     value, slices);
            }
        }
    }
    if (m_ShutdownRequested || m_Device->GetDeviceHealth() != DeviceHealth::Healthy)
    {
        Cancel(submit);
        return;
    }
    for (auto& tracked : submit.Buffers)
    {
        tracked->Buffer->commit();
        tracked->Buffer->release();
    }
    if (!submit.Signals.empty() && m_Queue != nullptr)
    {
        MTL::CommandBuffer* signalBuffer = m_Queue->commandBuffer();
        if (signalBuffer != nullptr)
        {
            m_Device->TrackFrameCommandBuffer(signalBuffer);
            for (auto& [event, value] : submit.Signals)
            {
                signalBuffer->encodeSignalEvent(event, value);
            }
            signalBuffer->commit();
        }
        else
        {
            m_Device->RecordSubmissionFailure();
        }
        for (auto& [event, value] : submit.Signals)
        {
            event->release();
        }
    }
    for (auto& [event, value] : submit.Waits)
        event->release();
    submit.Waits.clear();
    submit.Buffers.clear();
    submit.Signals.clear();
}

void MetalSubmitQueue::Cancel(Submit& submit)
{
    for (auto& tracked : submit.Buffers)
    {
        if (!tracked->RetirementClaimed.exchange(true, std::memory_order_acq_rel))
        {
            m_Device->RetireTrackedCommandBuffer(*tracked, nullptr);
        }
        tracked->Buffer->release();
    }
    for (auto& [event, value] : submit.Waits)
        event->release();
    for (auto& [event, value] : submit.Signals)
        event->release();
    submit.Buffers.clear();
    submit.Waits.clear();
    submit.Signals.clear();
}

} // namespace Rendering
} // namespace GameEngine
