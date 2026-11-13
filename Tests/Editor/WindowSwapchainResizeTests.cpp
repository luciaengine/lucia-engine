#include "Display/WindowSwapchainResize.h"

#include <gtest/gtest.h>

namespace
{

using GameEngine::Editor::WindowSwapchainResize;

constexpr uint32_t kSizeAWidth = 1920;
constexpr uint32_t kSizeAHeight = 1080;
constexpr uint32_t kSizeBWidth = 1280;
constexpr uint32_t kSizeBHeight = 720;
constexpr uint32_t kSizeCWidth = 2560;
constexpr uint32_t kSizeCHeight = 1440;

TEST(WindowSwapchainResizeTests, AResizeStormBackToTheSwapchainSizeRecreatesNothing)
{
    WindowSwapchainResize resize;
    resize.Mark(); // A -> B
    resize.Mark(); // B -> A

    EXPECT_FALSE(resize.TakeRecreate(kSizeAWidth, kSizeAHeight, kSizeAWidth, kSizeAHeight));
    EXPECT_FALSE(resize.TakeRecreate(kSizeCWidth, kSizeCHeight, kSizeAWidth, kSizeAHeight));
}

TEST(WindowSwapchainResizeTests, AResizeStormToANewSizeRecreatesOnceAtTheLastSize)
{
    WindowSwapchainResize resize;
    resize.Mark(); // A -> B
    resize.Mark(); // B -> C

    EXPECT_TRUE(resize.TakeRecreate(kSizeCWidth, kSizeCHeight, kSizeAWidth, kSizeAHeight));
}

TEST(WindowSwapchainResizeTests, AZeroSizeKeepsTheMarkUntilANonZeroSize)
{
    WindowSwapchainResize resize;
    resize.Mark();

    EXPECT_FALSE(resize.TakeRecreate(0, 0, kSizeAWidth, kSizeAHeight));
    EXPECT_FALSE(resize.TakeRecreate(kSizeBWidth, 0, kSizeAWidth, kSizeAHeight));
    EXPECT_TRUE(resize.TakeRecreate(kSizeBWidth, kSizeBHeight, kSizeAWidth, kSizeAHeight));
}

TEST(WindowSwapchainResizeTests, ASecondTakeAfterARecreateReturnsFalse)
{
    WindowSwapchainResize resize;
    resize.Mark();

    EXPECT_TRUE(resize.TakeRecreate(kSizeBWidth, kSizeBHeight, kSizeAWidth, kSizeAHeight));
    EXPECT_FALSE(resize.TakeRecreate(kSizeBWidth, kSizeBHeight, kSizeAWidth, kSizeAHeight));
}

} // namespace
