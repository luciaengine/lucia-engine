#pragma once

#include "UI/UIElement.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace GameEngine::Editor
{

// Which UI element instances a panel has already set up. A .uxml reconcile keeps an
// unchanged element (same object, same handlers) and replaces a changed one, so a
// panel that re-runs its binding after a reconcile acts only on instances it has not
// seen: re-registering a handler on a kept element would make it fire twice. Instance
// ids are never reused, so a recorded id stays a valid mark after its element is gone.
//
// A pass sees the ledger as it was when the pass began: two blocks of one pass that
// set up the same new element both get it. EndPass records what the pass saw.
class ElementInstanceLedger
{
  public:
    // True when element is non-null and no earlier pass recorded it.
    bool IsNew(const UIElement* element)
    {
        if (!element)
            return false;
        const std::uint64_t id = element->GetInstanceId();
        if (m_Recorded.count(id) != 0)
            return false;
        m_ThisPass.push_back(id);
        return true;
    }

    void EndPass()
    {
        m_Recorded.insert(m_ThisPass.begin(), m_ThisPass.end());
        m_ThisPass.clear();
    }

  private:
    std::unordered_set<std::uint64_t> m_Recorded;
    std::vector<std::uint64_t> m_ThisPass;
};

} // namespace GameEngine::Editor
