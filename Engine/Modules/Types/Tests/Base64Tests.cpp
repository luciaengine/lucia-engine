#include "Types/Base64.h"

#include <gtest/gtest.h>

using namespace GameEngine;

// RFC 4648 section 10 test vectors: every padding case.
TEST(Base64Tests, EncodesTheRfcVectors)
{
    EXPECT_EQ(Base64Encode(""), "");
    EXPECT_EQ(Base64Encode("f"), "Zg==");
    EXPECT_EQ(Base64Encode("fo"), "Zm8=");
    EXPECT_EQ(Base64Encode("foo"), "Zm9v");
    EXPECT_EQ(Base64Encode("foob"), "Zm9vYg==");
    EXPECT_EQ(Base64Encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(Base64Encode("foobar"), "Zm9vYmFy");
}

TEST(Base64Tests, EncodesBytesAboveSeven)
{
    const uint8_t bytes[] = {0x89, 0x50, 0x4E, 0x47, 0xFF, 0xFE};
    EXPECT_EQ(Base64Encode(bytes, sizeof(bytes)), "iVBOR//+");
}
