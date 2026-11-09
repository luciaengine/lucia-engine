#include "Mathematics/RectInt.h"

#include <gtest/gtest.h>

using GameEngine::Mathematics::RectInt;

TEST(RectIntTests, ContainsTheLeftAndTopEdgesButNotTheRightAndBottomEdges)
{
    const RectInt rect{10, 20, 30, 40};
    EXPECT_EQ(rect.Right(), 40);
    EXPECT_EQ(rect.Bottom(), 60);
    EXPECT_TRUE(rect.Contains(10, 20));
    EXPECT_TRUE(rect.Contains(39, 59));
    EXPECT_FALSE(rect.Contains(40, 30));
    EXPECT_FALSE(rect.Contains(20, 60));
    EXPECT_FALSE(rect.Contains(9, 30));
    EXPECT_FALSE(RectInt{}.Contains(0, 0));
}

TEST(RectIntTests, EqualityComparesEveryField)
{
    EXPECT_EQ((RectInt{1, 2, 3, 4}), (RectInt{1, 2, 3, 4}));
    EXPECT_NE((RectInt{1, 2, 3, 4}), (RectInt{9, 2, 3, 4}));
    EXPECT_NE((RectInt{1, 2, 3, 4}), (RectInt{1, 9, 3, 4}));
    EXPECT_NE((RectInt{1, 2, 3, 4}), (RectInt{1, 2, 9, 4}));
    EXPECT_NE((RectInt{1, 2, 3, 4}), (RectInt{1, 2, 3, 9}));
}
