#pragma once

#include "Types/FlatSet.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{
class UIElement;

namespace Editor
{

// Records the UI element instances that a panel has set up, so a bind after a layout reconcile
// acts only on new instances. A pass sees the ledger as it was at the start of the pass, and
// EndPass records what the pass saw. Instance ids are not reused.
class ElementInstanceLedger
{
  public:
    // Claims element for this pass. True when element is not null and no earlier pass claimed it.
    bool ClaimNew(const UIElement* element);
    void EndPass();

  private:
    FlatSet<std::uint64_t> m_Recorded;
    std::vector<std::uint64_t> m_ThisPass;
};

} // namespace Editor
} // namespace GameEngine
