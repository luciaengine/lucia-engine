// CPU checks of the 64-bit sort key of the sorted transparent path (SortedTransparentKey.h):
// the monotonic float transform, back-to-front order, priority over depth, and the index
// tiebreak that keeps equal-depth draws in a stable order.

#include <gtest/gtest.h>

#include "Engine/Rendering/SortedTransparentKey.h"

#include <cstdint>
#include <iterator>

namespace R = GameEngine::Engine::Renderer;

TEST(SortedTransparentKey, MonotonicFloatTransformOrdersLikeFloat)
{
    const float samples[] = {-1e30f, -100.0f, -1.5f, -0.0f, 0.0f, 0.5f, 1.0f, 100.0f, 1e30f};
    for (size_t i = 1; i < std::size(samples); ++i)
    {
        // Strictly increasing floats -> non-decreasing sortable uints.
        EXPECT_LE(R::FloatToSortableUint(samples[i - 1]), R::FloatToSortableUint(samples[i]))
            << "i=" << i;
    }
    // -0.0f orders immediately before +0.0f (a harmless, monotonically
    // consistent quirk of the transform), never after.
    EXPECT_LE(R::FloatToSortableUint(-0.0f), R::FloatToSortableUint(0.0f));
}

TEST(SortedTransparentKey, AscendingKeyOrderIsBackToFront)
{
    // Farther (larger viewDepth) must produce a SMALLER key so an ascending sort
    // draws it first (back-to-front).
    const auto near = R::MakeSortedTransparentKey(2.0f, 0, 0);
    const auto mid = R::MakeSortedTransparentKey(5.0f, 0, 0);
    const auto far = R::MakeSortedTransparentKey(9.0f, 0, 0);
    EXPECT_TRUE(R::SortedTransparentKeyLess(far, mid));
    EXPECT_TRUE(R::SortedTransparentKeyLess(mid, near));
}

TEST(SortedTransparentKey, EqualDepthBreaksByIndexDeterministically)
{
    const auto a = R::MakeSortedTransparentKey(4.0f, 0, 10);
    const auto b = R::MakeSortedTransparentKey(4.0f, 0, 20);
    // Same depth+priority -> lower index sorts first, and the order is total.
    EXPECT_TRUE(R::SortedTransparentKeyLess(a, b));
    EXPECT_FALSE(R::SortedTransparentKeyLess(b, a));
}

TEST(SortedTransparentKey, PriorityDominatesDepth)
{
    const auto lowPrioFar = R::MakeSortedTransparentKey(1000.0f, 0, 0);
    const auto highPrioNear = R::MakeSortedTransparentKey(0.1f, 1, 0);
    // Priority is the high field: priority 0 sorts entirely before priority 1
    // regardless of depth.
    EXPECT_TRUE(R::SortedTransparentKeyLess(lowPrioFar, highPrioNear));
}
