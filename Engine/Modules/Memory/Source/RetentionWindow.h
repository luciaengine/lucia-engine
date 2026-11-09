#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace GameEngine::Memory::Detail
{

/// The need of a thread's arena for its retention rule: the largest peak of the current
/// hysteresis window and of the previous one. A window closes after `kResetsPerWindow` resets,
/// so a block that one spike grew is released one to two windows after the spike.
class RetentionWindow
{
  public:
    static constexpr std::uint32_t kResetsPerWindow = 1024;

    /// Takes the peak of one reset into the current window.
    void Take(std::size_t peak) { m_WindowPeak = std::max(m_WindowPeak, peak); }

    /// The largest peak of the current window.
    std::size_t WindowPeak() const { return m_WindowPeak; }

    /// Counts `resets` more resets and returns the need, read before a window that they
    /// complete closes. `resets` divides `kResetsPerWindow`.
    std::size_t Advance(std::uint32_t resets)
    {
        const std::size_t need = std::max(m_WindowPeak, m_PreviousWindowPeak);
        m_Resets += resets;
        if (m_Resets == kResetsPerWindow)
        {
            m_PreviousWindowPeak = m_WindowPeak;
            m_WindowPeak = 0;
            m_Resets = 0;
        }
        return need;
    }

  private:
    std::size_t m_WindowPeak = 0;
    std::size_t m_PreviousWindowPeak = 0;
    std::uint32_t m_Resets = 0;
};

} // namespace GameEngine::Memory::Detail
