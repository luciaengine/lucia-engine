#include "UI/TextSegmentation.h"
#include "IsolatedUIFixture.h"
#include "Rendering/Text/TextLayout.h"
#include "../Source/TextSegmentation_Unicode.h"
#include <array>
#include <gtest/gtest.h>
#include <string>

using namespace GameEngine;

TEST(UnicodeTextEditing, CombiningMarksStayWithTheirBase)
{
    const std::string text = "a\xcc\x81" "b";
    EXPECT_EQ(TextSegmentation::NextGrapheme(text, 0), 3u);
    EXPECT_EQ(TextSegmentation::PrevGrapheme(text, 3), 0u);
    EXPECT_EQ(TextSegmentation::NextGrapheme(text, 3), 4u);
    EXPECT_EQ(TextSegmentation::PrevGrapheme(text, 4), 3u);
}

TEST(UnicodeTextEditing, JoinedEmojiAndFlagsAreSingleEditingUnits)
{
    for (const std::string cluster : {std::string("👨‍👩‍👧‍👦"), std::string("🇳🇱"), std::string("👍🏽")})
    {
        SCOPED_TRACE(cluster);
        const std::string text = "a" + cluster + "b";
        EXPECT_EQ(TextSegmentation::NextGrapheme(text, 1), 1 + cluster.size());
        EXPECT_EQ(TextSegmentation::PrevGrapheme(text, 1 + cluster.size()), 1u);
    }
}

TEST(UnicodeTextEditing, CarriageReturnLineFeedIsOneBoundary)
{
    EXPECT_EQ(TextSegmentation::NextGrapheme("a\r\nb", 1), 3u);
    EXPECT_EQ(TextSegmentation::PrevGrapheme("a\r\nb", 3), 1u);
    EXPECT_EQ(TextSegmentation::NextGrapheme("", 10), 0u);
    EXPECT_EQ(TextSegmentation::PrevGrapheme("abc", 0), 0u);
    EXPECT_EQ(TextSegmentation::NextGrapheme("abc", 30), 3u);
}

TEST(UnicodeTextEditing, BoundaryInsideCombiningSequenceUsesWholeCluster)
{
    const std::string text = "a\xcc\x81" "b";
    EXPECT_EQ(TextSegmentation::PrevGrapheme(text, 2), 0u);
    EXPECT_EQ(TextSegmentation::NextGrapheme(text, 2), 3u);
}

TEST(UnicodeTextEditing, ThaiWordSelectionUsesDictionaryBoundaries)
{
    const std::string text = "ภาษาไทย";
    const auto language = TextSegmentation::SegmentAt(text, 0);
    EXPECT_EQ(language.Begin, 0u);
    EXPECT_EQ(language.End, std::string("ภาษา").size());
    EXPECT_EQ(TextSegmentation::NextBoundary(text, 0), std::string("ภาษา").size());
    EXPECT_EQ(TextSegmentation::PrevBoundary(text, text.size()), std::string("ภาษา").size());
}

TEST(UnicodeTextEditing, NonAsciiDecimalDigitsRemainOneWord)
{
    const std::string text = "١٢٣٫٤٥";
    const auto number = TextSegmentation::SegmentAt(text, 4);
    EXPECT_EQ(number.Begin, 0u);
    EXPECT_EQ(number.End, text.size());
    EXPECT_EQ(TextSegmentation::NextBoundary(text, 0), text.size());
}

TEST(UnicodeTextEditing, UnicodeIdentifiersRetainEditorDotAndColonBoundaries)
{
    const std::string word = "κόσμος";
    for (const auto separator : {".", ":"})
    {
        const std::string text = word + separator + "txt";
        const auto selected = TextSegmentation::SegmentAt(text, 0);
        EXPECT_EQ(selected.Begin, 0u);
        EXPECT_EQ(selected.End, word.size());
        EXPECT_EQ(TextSegmentation::NextBoundary(text, 0), word.size()+1);
    }
}


TEST(UnicodeTextEditing, WordNavigationFollowsRightToLeftVisualPositions)
{
    const std::string text = "אב גד";
    const std::array<float, 10> positions{9, 9, 7, 7, 5, 4, 4, 2, 2, 0};
    const auto move = [&](size_t current, bool right) {
        return TextSegmentation::Detail::VisualWordBoundary(text, current, right, positions);
    };
    EXPECT_EQ(move(0, false), 5u);
    EXPECT_EQ(move(5, false), 9u);
    EXPECT_EQ(move(9, true), 5u);
    EXPECT_EQ(move(5, true), 0u);
    EXPECT_EQ(move(2, true), 0u);
    EXPECT_EQ(move(0, true), 0u);
}

TEST(UnicodeTextEditing, AsciiWordNavigationKeepsEditorPunctuationRules)
{
    const std::string text = "hello.world";
    const std::array<float, 12> positions{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    EXPECT_EQ(TextSegmentation::Detail::VisualWordBoundary(text, 0, true, positions), 6u);
    EXPECT_EQ(TextSegmentation::Detail::VisualWordBoundary(text, 11, false, positions), 6u);
}

TEST(UnicodeTextEditing, JapaneseLayoutHeightMatchesRenderedWrapping)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f,
        R"(<uielement id="root"><label id="text">冒険の世界へようこそ</label></uielement>)",
        R"(#root { width: 300px; height: 300px; }
            label { width: 60px; font-family: Roboto; font-size: 20px; line-height: 24px; })");
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    const auto* style = fixture.Style("text");
    ASSERT_NE(style, nullptr);
    auto* font = fixture.Manager().ResolveFontForStyle(*style);
    ASSERT_NE(font, nullptr);
    Rendering::Text::StyledRun run{font, "冒険の世界へようこそ", 20.0f};
    const auto rendered = Rendering::Text::TextLayout::ShapeMultiline(
        std::span<const Rendering::Text::StyledRun>(&run, 1), 60.0f, 24.0f);
    ASSERT_GT(rendered.LineCount, 1u);
    EXPECT_NEAR(fixture.BorderBox("text").H, rendered.Metrics.height, 0.01f);
}
