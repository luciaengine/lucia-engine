#include "Display/WindowSwapchainResize.h"

namespace GameEngine
{
namespace Editor
{

void WindowSwapchainResize::Mark()
{
    m_Marked = true;
}

bool WindowSwapchainResize::TakeRecreate(uint32_t windowWidth,
                                         uint32_t windowHeight,
                                         uint32_t swapchainWidth,
                                         uint32_t swapchainHeight)
{
    if (!m_Marked || windowWidth == 0 || windowHeight == 0)
        return false;

    m_Marked = false;
    return windowWidth != swapchainWidth || windowHeight != swapchainHeight;
}

} // namespace Editor
} // namespace GameEngine
