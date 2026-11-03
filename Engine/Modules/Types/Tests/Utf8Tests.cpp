#include "Types/Utf8.h"

#include <gtest/gtest.h>

using namespace GameEngine;

TEST(Utf8Tests, AcceptsWellFormedText)
{
    EXPECT_TRUE(IsValidUtf8(""));
    EXPECT_TRUE(IsValidUtf8("plain ascii\n"));
    EXPECT_TRUE(IsValidUtf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
}

TEST(Utf8Tests, RefusesMalformedText)
{
    EXPECT_FALSE(IsValidUtf8("\x80"));                 // stray continuation byte
    EXPECT_FALSE(IsValidUtf8("\xC3"));                 // truncated sequence
    EXPECT_FALSE(IsValidUtf8("\xC0\xAF"));             // overlong '/'
    EXPECT_FALSE(IsValidUtf8("\xED\xA0\x80"));         // surrogate U+D800
    EXPECT_FALSE(IsValidUtf8("\xF4\x90\x80\x80"));     // above U+10FFFF
    EXPECT_FALSE(IsValidUtf8("\x89PNG\r\n\x1A\n\xFF")); // binary
}

TEST(Utf8Tests, PrefixNeverSplitsACharacter)
{
    const std::string_view text = "a\xC3\xA9" "b"; // a, e-acute (2 bytes), b
    EXPECT_EQ(Utf8PrefixLength(text, 10), 4u);
    EXPECT_EQ(Utf8PrefixLength(text, 3), 3u);
    EXPECT_EQ(Utf8PrefixLength(text, 2), 1u); // would cut the e-acute
    EXPECT_EQ(Utf8PrefixLength(text, 1), 1u);
    EXPECT_EQ(Utf8PrefixLength(text, 0), 0u);
}
