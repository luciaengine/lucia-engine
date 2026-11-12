#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace GameEngine::Editor
{
// The deferred response owns its callback. Ending the response cancels queued
// declarations without keeping a closed window or a readback request alive.
class WindowRenderCallbackQueue
{
  public:
    using Callback = std::function<void(uint64_t)>;

    void Enqueue(uint64_t windowId, const std::shared_ptr<Callback>& callback)
    {
        m_Entries.push_back({windowId, callback});
    }

    bool HasPending(uint64_t windowId) const
    {
        return std::any_of(m_Entries.begin(), m_Entries.end(), [windowId](const Entry& entry)
        {
            return entry.WindowId == windowId && !entry.Function.expired();
        });
    }

    // Called only at this window's live pre-Execute seam. Callbacks enqueued
    // during dispatch wait for a later seam, including those for this window.
    void Dispatch(uint64_t windowId)
    {
        auto pending = std::move(m_Entries);
        m_Entries.clear();
        std::vector<std::weak_ptr<Callback>> ready;
        for (auto& entry : pending)
        {
            if (entry.Function.expired())
                continue;
            if (entry.WindowId == windowId)
                ready.push_back(std::move(entry.Function));
            else
                m_Entries.push_back(std::move(entry));
        }
        for (const auto& entry : ready)
            if (const auto callback = entry.lock())
                (*callback)(windowId);
    }

  private:
    struct Entry
    {
        uint64_t WindowId;
        std::weak_ptr<Callback> Function;
    };
    std::vector<Entry> m_Entries;
};
} // namespace GameEngine::Editor
