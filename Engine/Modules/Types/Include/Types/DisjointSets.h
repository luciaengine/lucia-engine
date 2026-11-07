#pragma once

#include "Types/Types.h"

#include <cstddef>
#include <numeric>
#include <vector>

namespace GameEngine
{

/// Disjoint sets over the items 0 to count - 1 (union-find). Each item starts in a set of its own.
/// Every link points to a smaller item, so the root of a set is its smallest item for any order of joins.
/// Find halves the path in a loop, so a long chain is cheap and cannot overflow the stack.
class DisjointSets
{
  public:
    DisjointSets() = default;
    explicit DisjointSets(std::size_t count) { Reset(count); }

    /// Puts each item 0 to count - 1 in a set of its own.
    /// Keeps the storage, so a count at or below the largest count so far does not allocate.
    void Reset(std::size_t count)
    {
        m_Parent.resize(count);
        std::iota(m_Parent.begin(), m_Parent.end(), 0u);
    }

    /// The smallest item of the set that holds `item`.
    uint32 Find(uint32 item) noexcept
    {
        while (m_Parent[item] != item)
        {
            m_Parent[item] = m_Parent[m_Parent[item]];
            item = m_Parent[item];
        }
        return item;
    }

    /// Merges the sets of `first` and `second`. The larger root links under the smaller root.
    void Join(uint32 first, uint32 second) noexcept
    {
        const uint32 firstRoot = Find(first);
        const uint32 secondRoot = Find(second);
        if (firstRoot < secondRoot)
            m_Parent[secondRoot] = firstRoot;
        else
            m_Parent[firstRoot] = secondRoot;
    }

  private:
    std::vector<uint32> m_Parent;
};

} // namespace GameEngine
