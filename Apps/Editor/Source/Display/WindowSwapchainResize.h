#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Editor
{

/// Decides when a window's swapchain is recreated after a framebuffer resize.
///
/// A resize only marks the window. The next frame takes the mark and recreates the
/// swapchain once, at the window size of that frame. Sizes that change and come back
/// between two frames cost no recreation.
class WindowSwapchainResize
{
public:
    /// Records a framebuffer resize. Several marks before the next take count as one.
    void Mark();

    /// Returns true once per mark when the swapchain must be recreated at the window size.
    /// Returns false when there is no mark, or when the window size equals the swapchain
    /// size (this clears the mark). A zero window size keeps the mark for a later frame.
    bool TakeRecreate(uint32_t windowWidth, uint32_t windowHeight, uint32_t swapchainWidth, uint32_t swapchainHeight);

private:
    bool m_Marked = false;
};

} // namespace Editor
} // namespace GameEngine
