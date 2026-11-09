#include <gtest/gtest.h>

#include "Types/StringUtils.h"

#include <cstring>

using namespace GameEngine;

TEST(StringUtilsTests, TrimRemovesEveryAsciiWhitespaceKindFromBothEnds)
{
    EXPECT_EQ(TrimWhitespaceView(" \t\r\n\v\fvalue \t\r\n\v\f"), "value");
    EXPECT_EQ(TrimWhitespace(" \t\r\n\v\fvalue \t\r\n\v\f"), "value");
}

TEST(StringUtilsTests, TrimKeepsInteriorWhitespace)
{
    EXPECT_EQ(TrimWhitespaceView("  a b\tc  "), "a b\tc");
    EXPECT_EQ(TrimWhitespace("  a b\tc  "), "a b\tc");
}

TEST(StringUtilsTests, TrimOfBlankTextIsEmpty)
{
    EXPECT_TRUE(TrimWhitespaceView(" \r\n\t").empty());
    EXPECT_TRUE(TrimWhitespace(" \r\n\t").empty());
    EXPECT_TRUE(TrimWhitespaceView("").empty());
}

TEST(StringUtilsTests, TrimOfCrlfLineDropsTheLineEnding)
{
    EXPECT_EQ(TrimWhitespaceView("key = value\r\n"), "key = value");
}

TEST(StringUtilsTests, ToLowerAsciiLowersOnlyAsciiLetters)
{
    EXPECT_EQ(ToLowerAscii(".PNG"), ".png");
    EXPECT_EQ(ToLowerAscii("Mixed_Case-123"), "mixed_case-123");
    EXPECT_EQ(ToLowerAscii("\xC3\x89"), "\xC3\x89") << "UTF-8 bytes pass through unchanged";
}

TEST(StringUtilsTests, CopyToFixedStringZeroFillsAfterTheText)
{
    char buffer[8];
    std::memset(buffer, 'x', sizeof(buffer));
    CopyToFixedString(buffer, "abc");
    EXPECT_STREQ(buffer, "abc");
    for (std::size_t i = 3; i < sizeof(buffer); ++i)
        EXPECT_EQ(buffer[i], '\0') << "byte " << i;
}

TEST(StringUtilsTests, CopyToFixedStringTruncatesAndKeepsTheTerminator)
{
    char buffer[4];
    CopyToFixedString(buffer, "abcdef");
    EXPECT_STREQ(buffer, "abc");
}

TEST(StringUtilsTests, CopyToFixedStringOfEmptyTextClearsTheBuffer)
{
    char buffer[4] = {'a', 'b', 'c', '\0'};
    CopyToFixedString(buffer, "");
    EXPECT_STREQ(buffer, "");
    EXPECT_EQ(buffer[2], '\0');
}

TEST(StringUtilsTests, StartsWithIgnoreCaseComparesThePrefixOnly)
{
    EXPECT_TRUE(StartsWithIgnoreCase("Offset = (1, 2, 3)", "offset"));
    EXPECT_FALSE(StartsWithIgnoreCase("off", "offset"));
    EXPECT_TRUE(StartsWithIgnoreCase("anything", ""));
}

// The compares fold A-Z only, without the C locale, so they are constant expressions.
static_assert(EqualsIgnoreCase("transformScaleY", "TRANSFORMSCALEY") && !EqualsIgnoreCase("\xC9", "\xE9"));
static_assert(StartsWithIgnoreCase("ValueCurve.target", "VALUECURVE.") && EndsWithIgnoreCase("level.SCENE", ".scene"));

TEST(StringUtilsTests, EqualsIgnoreCaseMatchesTheSameTextInAnyCase)
{
    EXPECT_TRUE(EqualsIgnoreCase("off", "OFF"));
    EXPECT_TRUE(EqualsIgnoreCase("False", "fALSE"));
    EXPECT_TRUE(EqualsIgnoreCase("", ""));
}

TEST(StringUtilsTests, EqualsIgnoreCaseRejectsPrefixesAndOtherText)
{
    EXPECT_FALSE(EqualsIgnoreCase("of", "off")) << "a prefix is not equal";
    EXPECT_FALSE(EqualsIgnoreCase("offset", "off")) << "neither is a longer text";
    EXPECT_FALSE(EqualsIgnoreCase("no", "on"));
    EXPECT_FALSE(EqualsIgnoreCase("", "0"));
}

TEST(StringUtilsTests, ContainsControlCharacterFindsBytesBelowSpaceAndDelete)
{
    EXPECT_TRUE(ContainsControlCharacter(std::string("a\0b", 3)));
    EXPECT_TRUE(ContainsControlCharacter("tab\there"));
    EXPECT_TRUE(ContainsControlCharacter("del\x7F"));
    EXPECT_FALSE(ContainsControlCharacter("Textures/Bloom/lens Dirt1.png"));
    EXPECT_FALSE(ContainsControlCharacter("caf\xC3\xA9.png")) << "UTF-8 bytes are not control characters";
    EXPECT_FALSE(ContainsControlCharacter(""));
}

// Reflected inspector rows and options are labelled from their identifiers.
TEST(StringUtilsTests, IdentifierToWordsReadsIdentifiersAsWords)
{
    struct Case
    {
        const char* Identifier;
        const char* Words;
    };
    const Case cases[] = {
        // The owner's cases (#2231), verbatim.
        {"PrewarmSeconds", "Prewarm Seconds"},
        {"castsShadows", "Casts Shadows"},
        {"m_Value", "Value"},
        {"value_", "Value"},
        {"variable_name", "Variable Name"},
        {"GPUSkin", "GPU Skin"},
        {"URLPath", "URL Path"},
        {"CastsShadows", "Casts Shadows"},
        {"initial_linear_velocity", "Initial Linear Velocity"},
        {"m_SheetColumns", "Sheet Columns"},
        {"_private_", "Private"},
        {"trailing_", "Trailing"},
        {"HDRColor", "HDR Color"},
        // An acronym keeps its number, a dimension stays whole, a two-letter acronym before a
        // capital word splits from it once.
        {"MSM4", "MSM4"},
        {"HDR10", "HDR10"},
        {"Grid5x5", "Grid 5x5"},
        {"Grid3x3", "Grid 3x3"},
        {"FidelityFXFast", "Fidelity FX Fast"},
        {"SSAO", "SSAO"},
        {"World3D", "World 3D"},
        {"Cascaded2", "Cascaded 2"},
        {"Mip0Bias", "Mip 0 Bias"},
        {"Cascade16Count", "Cascade 16 Count"},
        {"FaceCameraYAlongVelocity", "Face Camera Y Along Velocity"},
        {"x", "X"},
        {"Already Words", "Already Words"},
        {"", ""},
    };
    for (const Case& c : cases)
        EXPECT_EQ(IdentifierToWords(c.Identifier), c.Words) << c.Identifier;
}

TEST(StringUtilsTests, IsValidUtf8TakesWellFormedTextOnly)
{
    EXPECT_TRUE(IsValidUtf8(""));
    EXPECT_TRUE(IsValidUtf8("readme.txt"));
    EXPECT_TRUE(IsValidUtf8("caf\xC3\xA9/\xE6\x97\xA5\xE6\x9C\xAC/\xF0\x9F\x98\x80.png")) << "two, three and four bytes";
    EXPECT_TRUE(IsValidUtf8("\xF4\x8F\xBF\xBF")) << "U+10FFFF";
    EXPECT_FALSE(IsValidUtf8("caf\x82.txt")) << "code page 437";
    EXPECT_FALSE(IsValidUtf8("\xC0\xAF")) << "overlong '/'";
    EXPECT_FALSE(IsValidUtf8("\xE0\x80\xAF")) << "overlong '/'";
    EXPECT_FALSE(IsValidUtf8("a\x80" "b")) << "lone continuation byte";
    EXPECT_FALSE(IsValidUtf8("caf\xC3")) << "truncated at the end";
    EXPECT_FALSE(IsValidUtf8("\xE6\x97.txt")) << "truncated before ASCII";
    EXPECT_FALSE(IsValidUtf8("\xED\xA0\x80")) << "surrogate half";
    EXPECT_FALSE(IsValidUtf8("\xF4\x90\x80\x80")) << "past U+10FFFF";
}

TEST(StringUtilsTests, UnqualifiedNameKeepsTheNameAfterTheLastQualifier)
{
    EXPECT_EQ(UnqualifiedName("GameEngine::Components::Light"), "Light");
    EXPECT_EQ(UnqualifiedName("MyGame.Combat.HealthPool"), "HealthPool");
    EXPECT_EQ(UnqualifiedName("Light"), "Light");
    EXPECT_EQ(UnqualifiedName(""), "");
}

TEST(StringUtilsTests, ToUpperAsciiRaisesOnlyAsciiLetters)
{
    EXPECT_EQ(ToUpperAscii("abc-Def_9"), "ABC-DEF_9");
    EXPECT_EQ(ToUpperAscii(""), "");
}

TEST(StringUtilsTests, ReplaceAllReplacesEveryOccurrenceOnce)
{
    EXPECT_EQ(ReplaceAll("@NAME@ and @NAME@", "@NAME@", "Game"), "Game and Game");
    EXPECT_EQ(ReplaceAll("aa", "a", "aa"), "aaaa") << "a replacement is not scanned again";
    EXPECT_EQ(ReplaceAll("abc", "", "x"), "abc") << "an empty pattern changes nothing";
    EXPECT_EQ(ReplaceAll("abc", "d", "x"), "abc");
    EXPECT_EQ(ReplaceAll("a.b.c", ".", ""), "abc");
}

TEST(StringUtilsTests, CountOccurrencesCountsWithoutOverlap)
{
    EXPECT_EQ(CountOccurrences("abc", "d"), 0u);
    EXPECT_EQ(CountOccurrences("abc", "b"), 1u);
    EXPECT_EQ(CountOccurrences("<path/><path/><path/>", "<path"), 3u);
    EXPECT_EQ(CountOccurrences("aaaa", "aa"), 2u) << "a match does not overlap the one before it";
    EXPECT_EQ(CountOccurrences("abc", ""), 0u) << "an empty needle counts nothing";
}
