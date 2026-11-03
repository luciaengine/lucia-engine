#include "Logger/Logger.h"
#include "Logger/RingBufferSink.h"
#include "Platform/Shell.h"
#include "TestTempDir.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using GameEngine::Rendering::Text::FontAtlas;

static std::filesystem::path TestFontPath(const std::filesystem::path& file)
{
    return GameEngine::Platform::GetExecutablePath().parent_path() / "TestData/Text" / file;
}

static std::string FindFallbackFont()
{
    return TestFontPath("Roboto-Regular.ttf").string();
}

static bool LoadFontBytes(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

TEST(FontAtlasShapingTests, KerningReducesSpacingForAV)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    auto mA = fa.MeasureUtf8("A", 48);
    auto mV = fa.MeasureUtf8("V", 48);
    auto mAV = fa.MeasureUtf8("AV", 48);

    EXPECT_LE(mAV.width, mA.width + mV.width + 1.0f);
}

TEST(FontAtlasShapingTests, LigatureMayReduceGlyphCountForFFI)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    FontAtlas::ShapeResult result;
    fa.ShapeText("ffi", 48, result);
    EXPECT_LE(result.glyphs.size(), (size_t)3);
    EXPECT_GT(result.glyphs.size(), 0u);
}

TEST(FontAtlasShapingTests, ArabicRTLProducesGlyphs)
{
    FontAtlas fa;
    auto font = TestFontPath("NotoSansArabic-Regular.ttf").string();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    const char* arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85";
    FontAtlas::ShapeResult result;
    fa.ShapeText(arabic, 48, result);
    EXPECT_GT(result.glyphs.size(), 0u);
}

TEST(FontAtlasShapingTests, LineMetricsStayInLockstepWithMeasureUtf8)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    // Render sizes are float px: the text stack measures at the fractional
    // device size rather than a rounded integer.
    const float kRenderSizes[] = {10.0f, 14.0f, 24.0f, 32.0f};
    const char* text = "0.000";

    for (float pixelSize : kRenderSizes)
    {
        auto metrics = fa.MeasureUtf8(text, pixelSize);
        auto lm = fa.GetFontLineMetrics(pixelSize);

        EXPECT_GT(metrics.ascender, 0.0f);
        EXPECT_GT(metrics.descender, 0.0f);
        EXPECT_GT(lm.ascender, 0.0f);
        EXPECT_GT(lm.descender, 0.0f);

        const float kEps = 1.0f;
        EXPECT_NEAR(metrics.ascender, lm.ascender, kEps);
        EXPECT_NEAR(metrics.descender, lm.descender, kEps);
        EXPECT_NEAR(metrics.baseline, lm.ascender, kEps);
    }
}

// Caret positions are compared only against the advance total, never against glyph ink:
// the two are not comparable in either direction. GlyphPlacement.x/.width is the rendered
// quad (screen-space, bearing included), while metrics.width and caretXByByte accumulate
// advances, so the gap between them is the trailing glyph's right side bearing -- a
// quantity a font is free to give either sign. On segoeui, ink falls 1.13px short of
// advance for the "0.000 gp" below at 24px (the trailing p has positive bearing), yet
// overhangs it by 0.72px for "f" at 48px; the overhang holds for every font
// FindFallbackFont can pick on Windows (arial 1.66px, verdana 1.55px at 48px) and reaches
// 9.27px on timesi. Whichever direction is asserted, some string and font pair falsifies it.
TEST(FontAtlasShapingTests, CaretMapAlignsWithMetrics)
{
    FontAtlas fa;
    auto font = FindFallbackFont();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(font, bytes));
    ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), 48));

    const float kPixelSize = 24.0f;
    const char* text = "0.000 gp";

    auto measure = fa.MeasureText(text, kPixelSize);
    FontAtlas::ShapeResult shaped;
    fa.ShapeText(text, kPixelSize, shaped);

    ASSERT_GT(shaped.glyphs.size(), 0u);
    ASSERT_FALSE(measure.caretXByByte.empty());

    const auto& caret = measure.caretXByByte;
    for (size_t i = 1; i < caret.size(); ++i)
    {
        EXPECT_LE(caret[i - 1], caret[i] + 1e-4f);
    }

    const float kWidthEps = 0.5f;
    EXPECT_NEAR(measure.metrics.width, caret.back(), kWidthEps);
}

#if defined(GE_HAVE_FREETYPE)
// Glyph metrics in font units, read straight from FreeType with the same
// convention the Slug bounds are built from (FT_LOAD_NO_SCALE, unhinted) — but
// read independently of ShapeText, so an expected placement never derives from
// the placement under test.
struct FontUnitGlyphMetrics
{
    long BearingX = 0; // ink xMin
    long Width = 0;    // ink xMax - xMin
    long Advance = 0;
    long InkRight() const { return BearingX + Width; }
    long Rsb() const { return Advance - InkRight(); } // right side bearing; either sign
};

static bool ReadFontUnitMetrics(FT_Face face, uint32_t codepoint, FontUnitGlyphMetrics& out)
{
    const FT_UInt gid = FT_Get_Char_Index(face, (FT_ULong)codepoint);
    if (gid == 0)
        return false;
    if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
        return false;
    out.BearingX = (long)face->glyph->metrics.horiBearingX;
    out.Width = (long)face->glyph->metrics.width;
    out.Advance = (long)face->glyph->metrics.horiAdvance;
    return true;
}
#endif

// The ink-quad/advance relation that IS invariant, per glyph: the ink quad sits
// at the glyph's caret position plus its bearing. In render pixels, with
// S = rasterPpem / unitsPerEm and rsb = advance - (bearingX + width):
//   inkLeft  == caret + bearingX * S
//   inkRight == caret + (advance - rsb) * S
// so |inkRight - caretEnd| is bounded by the glyph's own |rsb|, never by a
// tuned constant. Expected values come from FreeType font-unit metrics
// (FontUnitGlyphMetrics above); kerning cancels because ink placement never
// reads the advance. kPlacementTolPx covers float rounding only: caret map and
// pen accumulate the same 26.6 advances from the same hb_font, and the
// atlas-to-render scales include fractional sizes. The
// smallest |rsb| in this population is two orders of magnitude above it.
//
// The fonts are chosen so BOTH rsb signs are present ('f' overhangs its
// advance in every candidate; digits and 'p' leave a positive gap) — with one
// sign the bound could never fail in the direction ink-vs-advance comparisons
// historically got wrong, and the test would be vacuous.
TEST(FontAtlasShapingTests, GlyphPlacementSitsAtCaretPlusBearing)
{
#if !defined(GE_HAVE_FREETYPE)
    GTEST_SKIP() << "FreeType not available";
#else
    using GameEngine::Rendering::Text::FreeTypeFace;
    using GameEngine::Rendering::Text::FreeTypeLib;

    const std::string fontsDir = TestFontPath("").string();
    const char* kFontCandidates[] = {"Roboto-Regular.ttf", "Roboto-Italic.ttf"};

    const unsigned kAtlasPixelSize = 48;
    const float kRenderSizes[] = {24.0f, 24.5f, 37.25f, 48.0f};
    // ASCII and ligature-free (no f+f/i/l pairs), so glyphs map 1:1 to
    // non-space chars; "AV To" adds kerning pairs to stress caret agreement.
    const char* kFixtures[] = {"0.000 gp", "f", "AV To"};
    const float kPlacementTolPx = 1e-3f;

    bool sawNegativeRsb = false;
    bool sawPositiveRsb = false;
    int fontsLoaded = 0;

    for (const char* fontFile : kFontCandidates)
    {
        const std::string path = fontsDir + fontFile;
        if (!std::filesystem::exists(path))
            continue;
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(LoadFontBytes(path, bytes));
        FontAtlas fa;
        ASSERT_TRUE(fa.LoadFontBytes(bytes.data(), bytes.size(), kAtlasPixelSize));
        ++fontsLoaded;

        FreeTypeLib refLib;
        ASSERT_TRUE(refLib.Init());
        FreeTypeFace refFace;
        ASSERT_TRUE(refFace.NewMemoryFace(refLib.Lib(), bytes.data(), bytes.size()));
        ASSERT_GT(refFace.Face()->units_per_EM, 0);
        const double upem = (double)refFace.Face()->units_per_EM;

        for (float pixelSize : kRenderSizes)
        {
            // Quad geometry follows the fractional render size.
            const double S = (double)pixelSize / upem;

            int negCount = 0, posCount = 0;
            double rsbMinPx = 0.0, rsbMaxPx = 0.0;
            char rsbMinChar = ' ', rsbMaxChar = ' ';
            double maxAbsDeltaPx = 0.0;

            for (const char* text : kFixtures)
            {
                SCOPED_TRACE(std::string(fontFile) + " @" +
                             std::to_string((int)pixelSize) + "px \"" + text + "\"");
                auto measure = fa.MeasureText(text, pixelSize);
                FontAtlas::ShapeResult shaped;
                fa.ShapeText(text, pixelSize, shaped);

                const std::string_view sv(text);
                ASSERT_EQ(measure.caretXByByte.size(), sv.size() + 1);

                // One quad per non-space char: ShapeText emits no quad for
                // whitespace, and these fixtures shape without substitutions.
                // A mismatch here means a ligature or missing glyph, which
                // would break the quad-to-caret mapping below.
                std::vector<size_t> quadByteIndex;
                for (size_t b = 0; b < sv.size(); ++b)
                {
                    if (sv[b] != ' ')
                        quadByteIndex.push_back(b);
                }
                ASSERT_EQ(shaped.glyphs.size(), quadByteIndex.size());

                for (size_t k = 0; k < shaped.glyphs.size(); ++k)
                {
                    const auto& gp = shaped.glyphs[k];
                    const size_t b = quadByteIndex[k];
                    const char ch = sv[b];
                    FontUnitGlyphMetrics fu;
                    ASSERT_TRUE(ReadFontUnitMetrics(refFace.Face(), (uint32_t)(unsigned char)ch, fu));

                    const long rsb = fu.Rsb();
                    const double caret = measure.caretXByByte[b];
                    const double predictedLeft = caret + fu.BearingX * S;
                    const double predictedRight = caret + (fu.Advance - rsb) * S;
                    EXPECT_NEAR(gp.x, predictedLeft, kPlacementTolPx) << "glyph '" << ch << "'";
                    EXPECT_NEAR(gp.x + gp.width, predictedRight, kPlacementTolPx)
                        << "glyph '" << ch << "'";

                    const double deltaLeft = std::abs(gp.x - predictedLeft);
                    const double deltaRight = std::abs((gp.x + gp.width) - predictedRight);
                    maxAbsDeltaPx = std::max({maxAbsDeltaPx, deltaLeft, deltaRight});

                    const double rsbPx = rsb * S;
                    if (rsb < 0)
                        ++negCount;
                    else if (rsb > 0)
                        ++posCount;
                    sawNegativeRsb = sawNegativeRsb || rsb < 0;
                    sawPositiveRsb = sawPositiveRsb || rsb > 0;
                    if (rsbPx < rsbMinPx)
                    {
                        rsbMinPx = rsbPx;
                        rsbMinChar = ch;
                    }
                    if (rsbPx > rsbMaxPx)
                    {
                        rsbMaxPx = rsbPx;
                        rsbMaxChar = ch;
                    }
                }
            }
            std::printf("[ rsb      ] %-12s @%2.0fpx  neg=%d pos=%d  min=%+.4fpx('%c') "
                        "max=%+.4fpx('%c')  maxAbsDelta=%.6fpx\n",
                        fontFile, pixelSize, negCount, posCount, rsbMinPx, rsbMinChar,
                        rsbMaxPx, rsbMaxChar, maxAbsDeltaPx);
        }
    }
    ASSERT_GT(fontsLoaded, 0);
    EXPECT_TRUE(sawNegativeRsb) << "no negative right-side-bearing glyph measured";
    EXPECT_TRUE(sawPositiveRsb) << "no positive right-side-bearing glyph measured";
#endif
}

TEST(FontAtlasShapingTests, PackagedFallbackRendersMissingScripts)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    const std::pair<const char*, const char*> samples[] = {
        {"NotoSansArabic-Regular.ttf", "مرحبا"},
        {"NotoSansHebrew-Regular.ttf", "שלום"},
        {"NotoSansDevanagari-Regular.ttf", "नमस्ते"},
        {"NotoSansThai-Regular.ttf", "สวัสดี"},
        {"NotoSansJP-Regular.otf", "冒険へようこそ"},
    };
    for (const auto& [file, text] : samples)
    {
        SCOPED_TRACE(file);
        FontAtlas atlas;
        ASSERT_TRUE(atlas.SetFallbackFonts({TestFontPath(file)}));
        ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
        FontAtlas::ShapeResult shape;
        atlas.ShapeText(text, 32.5f, shape);
        EXPECT_GT(shape.glyphs.size(), 0u);
        EXPECT_GT(shape.metrics.width, 0.0f);
        EXPECT_NEAR(atlas.MeasureUtf8(text, 32.5f).width, shape.metrics.width, 0.01f);
        EXPECT_GT(atlas.GetSlugPageCount(), 0);
        for (const auto& glyph : shape.glyphs)
        {
            EXPECT_TRUE(std::isfinite(glyph.x));
            EXPECT_TRUE(std::isfinite(glyph.y));
            EXPECT_GT(glyph.width, 0.0f);
            EXPECT_LT(glyph.pageIndex, atlas.GetSlugPageCount());
        }
    }
}

TEST(FontAtlasShapingTests, RightToLeftCaretsFollowLogicalInsertionOrder)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("NotoSansHebrew-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    const std::string text = "שלום";
    auto measured = atlas.MeasureText(text, 32.0f);
    ASSERT_EQ(measured.caretXByByte.size(), text.size() + 1);
    EXPECT_NEAR(measured.caretXByByte.front(), measured.metrics.width, 0.01f);
    EXPECT_NEAR(measured.caretXByByte.back(), 0.0f, 0.01f);
    EXPECT_GT(measured.caretXByByte[2], measured.caretXByByte[4]);
}

TEST(FontAtlasShapingTests, MixedDirectionNumbersKeepMeasurementAndCaretsInBounds)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.SetFallbackFonts({TestFontPath("NotoSansHebrew-Regular.ttf")}));
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    const std::string text = "Play שלום 123 now";
    FontAtlas::ShapeResult shape;
    atlas.ShapeText(text, 32.0f, shape, 0xffffffff, 1.25f);
    const auto measure = atlas.MeasureText(text, 32.0f, 1.25f);
    EXPECT_NEAR(shape.metrics.width, measure.metrics.width, 0.01f);
    for (float caret : measure.caretXByByte)
    {
        EXPECT_GE(caret, -0.01f);
        EXPECT_LE(caret, measure.metrics.width + 0.01f);
    }
    EXPECT_GT(measure.caretXByByte[5], measure.caretXByByte[7]);
}

TEST(FontAtlasShapingTests, MixedDirectionSelectionUsesSeparateVisualRanges)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.SetFallbackFonts({TestFontPath("NotoSansHebrew-Regular.ttf")}));
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    const std::string text = "abc שלום xyz";
    const auto measured = atlas.MeasureText(text, 32);
    const auto ranges = measured.SelectionRanges(0, 6);
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_LT(ranges[0].second, ranges[1].first);
    const auto all = measured.SelectionRanges(0, text.size());
    ASSERT_EQ(all.size(), 1u);
    EXPECT_NEAR(all.front().first, 0.0f, 0.01f);
    EXPECT_NEAR(all.front().second, measured.metrics.width, 0.01f);
}

TEST(FontAtlasShapingTests, LanguageSwitchInvalidatesLayoutAndRejectsInvalidTag)
{
    FontAtlas atlas;
    const auto before = atlas.GetLayoutRevision();
    ASSERT_TRUE(atlas.SetTextLanguage("ja"));
    EXPECT_GT(atlas.GetLayoutRevision(), before);
    const auto japanese = atlas.GetLayoutRevision();
    EXPECT_TRUE(atlas.SetTextLanguage("ja"));
    EXPECT_EQ(atlas.GetLayoutRevision(), japanese);
    EXPECT_FALSE(atlas.SetTextLanguage("not a language"));
    EXPECT_EQ(atlas.GetLayoutRevision(), japanese);
    EXPECT_TRUE(atlas.SetTextLanguage("zh-Hant"));
    EXPECT_GT(atlas.GetLayoutRevision(), japanese);
}

TEST(FontAtlasShapingTests, UnicodeWrappingFindsJapaneseAndThaiBoundaries)
{
    using namespace GameEngine::Rendering::Text;
    const std::pair<const char*, const char*> samples[] = {
        {"NotoSansJP-Regular.otf", "冒険の世界へようこそ"},
        {"NotoSansThai-Regular.ttf", "ภาษาไทยภาษาไทยภาษาไทย"}
    };
    for (const auto& [file, text] : samples)
    {
        SCOPED_TRACE(file);
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(LoadFontBytes(TestFontPath(file).string(), bytes));
        FontAtlas atlas;
        ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
        StyledRun run{&atlas, text, 32.0f};
        const float width = atlas.MeasureUtf8(text, 32.0f).width * 0.55f;
        const auto lines = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1),
            width, 0.0f, TextLayout::WordBreak::Normal);
        EXPECT_GT(lines.LineCount, 1u);
        EXPECT_LE(lines.Metrics.width, width + 0.1f);
        ASSERT_FALSE(lines.LineBreaks.empty());
        EXPECT_EQ(lines.LineBreaks.front().ByteStart, 0u);
        EXPECT_EQ(lines.LineBreaks.back().ByteEnd, run.Text.size());
    }
}

TEST(FontAtlasShapingTests, NonbreakingSpaceAndStyledWordDoNotCreateWrapOpportunities)
{
    using namespace GameEngine::Rendering::Text;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(FindFallbackFont(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    StyledRun noBreak{&atlas, "hello\xc2\xa0world", 32.0f};
    EXPECT_EQ(TextLayout::ShapeMultiline(std::span<const StyledRun>(&noBreak, 1),
        40.0f, 0.0f, TextLayout::WordBreak::Normal).LineCount, 1u);
    const StyledRun spans[] = {{&atlas, "hello", 32.0f}, {&atlas, "world", 32.0f}};
    EXPECT_EQ(TextLayout::ShapeMultiline(spans, 40.0f, 0.0f,
        TextLayout::WordBreak::Normal).LineCount, 1u);
}

TEST(FontAtlasShapingTests, ExplicitLinesRetainLetterSpacing)
{
    using namespace GameEngine::Rendering::Text;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(FindFallbackFont(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    StyledRun run{&atlas, "AB\nCD", 32.0f};
    run.LetterSpacingPx = 4.0f;
    const auto lines = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), 0.0f, 0.0f);
    ASSERT_EQ(lines.LineBreaks.size(), 2u);
    EXPECT_NEAR(lines.LineBreaks[0].Width, atlas.MeasureUtf8("AB", 32.0f, 4.0f).width, 0.01f);
    EXPECT_NEAR(lines.LineBreaks[1].Width, atlas.MeasureUtf8("CD", 32.0f, 4.0f).width, 0.01f);
}

TEST(FontAtlasShapingTests, HebrewArrowMovementFollowsVisualDirection)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("NotoSansHebrew-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    const auto measured = atlas.MeasureText("שלום", 32.0f);
    EXPECT_EQ(measured.NextVisualCaret(0, false), 2u);
    EXPECT_EQ(measured.NextVisualCaret(2, true), 0u);
    EXPECT_EQ(measured.NextVisualCaret(0, true), 0u);
    EXPECT_EQ(measured.CaretAtX(0), 8u);
    EXPECT_EQ(measured.CaretAtX(measured.metrics.width), 0u);
}

TEST(FontAtlasShapingTests, VisualMovementNeverSplitsCombiningCluster)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    const auto measured = atlas.MeasureText("a\xcc\x81" "b", 32.0f);
    EXPECT_EQ(measured.NextVisualCaret(0, true), 3u);
    EXPECT_EQ(measured.NextVisualCaret(3, false), 0u);
    for (float x=0; x<measured.metrics.width; x+=0.5f)
    {
        const auto caret=measured.CaretAtX(x);
        EXPECT_TRUE(caret==0 || caret==3 || caret==4);
    }
}

TEST(FontAtlasShapingTests, MixedSizeRunsShareOneBaseline)
{
    using namespace GameEngine::Rendering::Text;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("NotoSans-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    const StyledRun runs[] = {{&atlas, "H", 16.0f}, {&atlas, "H", 40.0f}};
    FontAtlas::ShapeResult small, large;
    atlas.ShapeText("H", 16.0f, small);
    atlas.ShapeText("H", 40.0f, large);
    ASSERT_EQ(small.glyphs.size(), 1u);
    ASSERT_EQ(large.glyphs.size(), 1u);
    const auto line = TextLayout::ShapeMultiline(runs, 0.0f, 0.0f);
    ASSERT_EQ(line.Glyphs.size(), 2u);
    const float smallBaseline = line.Glyphs[0].y - small.glyphs[0].y + atlas.GetFontLineMetrics(16.0f).ascender;
    const float largeBaseline = line.Glyphs[1].y - large.glyphs[0].y + atlas.GetFontLineMetrics(40.0f).ascender;
    EXPECT_NEAR(smallBaseline, largeBaseline, 0.01f);
}

TEST(FontAtlasShapingTests, StyledHebrewSpansUseVisualOrder)
{
    using namespace GameEngine::Rendering::Text;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("NotoSansHebrew-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    StyledRun runs[] = {{&atlas, "אב", 32.0f}, {&atlas, "גד", 32.0f}};
    runs[0].Color[0]=1; runs[0].Color[1]=0; runs[0].Color[2]=0; runs[0].Color[3]=1;
    runs[1].Color[0]=0; runs[1].Color[1]=0; runs[1].Color[2]=1; runs[1].Color[3]=1;
    const auto line=TextLayout::ShapeMultiline(runs, 0.0f, 0.0f);
    ASSERT_EQ(line.Glyphs.size(), 4u);
    EXPECT_EQ(line.Glyphs.front().color, 0xffff0000u);
    EXPECT_EQ(line.Glyphs.back().color, 0xff0000ffu);
}

TEST(FontAtlasShapingTests, StyledArabicRangeRetainsJoiningContext)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("NotoSansArabic-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    FontAtlas::ShapeResult whole, first;
    atlas.ShapeText("بب", 32.0f, whole);
    atlas.ShapeTextRange("بب", 0, 2, 32.0f, first, 0xffffffffu, 0.0f);
    ASSERT_EQ(whole.glyphs.size(), 2u);
    ASSERT_EQ(first.glyphs.size(), 1u);
    EXPECT_EQ(first.glyphs[0].glyphLocX, whole.glyphs[1].glyphLocX);
    EXPECT_EQ(first.glyphs[0].glyphLocY, whole.glyphs[1].glyphLocY);
    EXPECT_EQ(first.glyphs[0].pageIndex, whole.glyphs[1].pageIndex);
}

TEST(FontAtlasShapingTests, StyledCombiningContinuationDoesNotEmitDuplicateText)
{
    using namespace GameEngine::Rendering::Text;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("NotoSans-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    const StyledRun runs[] = {{&atlas, "a", 32.0f}, {&atlas, "\xcc\x81", 32.0f}};
    const auto line = TextLayout::ShapeMultiline(runs, 0.0f, 0.0f);
    FontAtlas::ShapeResult whole, continuation;
    atlas.ShapeText("a\xcc\x81", 32.0f, whole);
    atlas.ShapeTextRange("a\xcc\x81", 1, 3, 32.0f, continuation, 0xffffffffu, 0.0f);
    EXPECT_TRUE(continuation.glyphs.empty());
    EXPECT_EQ(line.Glyphs.size(), whole.glyphs.size());
    EXPECT_NEAR(line.Metrics.width, whole.metrics.width, 0.01f);
}

TEST(FontAtlasShapingTests, FallbackConfigurationSeparatesAtlasIdentity)
{
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    FontAtlas primary, arabic, hebrew, sameArabic;
    ASSERT_TRUE(arabic.SetFallbackFonts({TestFontPath("NotoSansArabic-Regular.ttf")}));
    ASSERT_TRUE(sameArabic.SetFallbackFonts({TestFontPath("NotoSansArabic-Regular.ttf")}));
    ASSERT_TRUE(hebrew.SetFallbackFonts({TestFontPath("NotoSansHebrew-Regular.ttf")}));
    for (auto* atlas : {&primary, &arabic, &hebrew, &sameArabic})
        ASSERT_TRUE(atlas->LoadFontBytes(bytes.data(), bytes.size(), 48));
    EXPECT_NE(primary.GetAtlasId(), arabic.GetAtlasId());
    EXPECT_NE(arabic.GetAtlasId(), hebrew.GetAtlasId());
    EXPECT_EQ(arabic.GetAtlasId(), sameArabic.GetAtlasId());
    const auto identity = arabic.GetAtlasId();
    ASSERT_TRUE(arabic.SetTextLanguage("ar"));
    EXPECT_EQ(arabic.GetAtlasId(), identity); // Language does not replace glyph storage.
}

TEST(FontAtlasShapingTests, FallbackFontUnderNonAsciiDirectoryShapes)
{
    GameEngine::TestUtils::ScopedTempDir temp(GameEngine::TestUtils::MakeUniqueTempDirectory("font_fallback_unicode"));
    const auto directory = temp.Path() / std::filesystem::path(u8"Fonts \u00e9\u5192\u05e9");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    ASSERT_FALSE(error) << error.message();
    const auto fallback = directory / "NotoSansHebrew-Regular.ttf";
    std::filesystem::copy_file(TestFontPath("NotoSansHebrew-Regular.ttf"), fallback, error);
    ASSERT_FALSE(error) << error.message();
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.SetFallbackFonts({fallback}));
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    FontAtlas::ShapeResult shape;
    atlas.ShapeText("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 32.0f, shape);
    EXPECT_GT(shape.glyphs.size(), 0u);
    EXPECT_GT(shape.metrics.width, 0.0f);
}

TEST(FontAtlasShapingTests, MissingGlyphWarningIsEmittedOncePerFamilyAndScript)
{
    Logger::Log::Initialize({});
    struct ShutdownLog
    {
        ~ShutdownLog() { Logger::Log::Shutdown(); }
    } shutdown;
    Logger::Log::ClearSinks();
    auto sink = std::make_unique<Logger::RingBufferSink>(16);
    auto* ring = sink.get();
    Logger::Log::AddSink(std::move(sink));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    FontAtlas::ShapeResult shape;
    atlas.ShapeText("\xe5\x86\x92", 32.0f, shape);
    EXPECT_TRUE(shape.glyphs.empty());
    atlas.ShapeText("\xe9\x99\xba", 32.0f, shape);
    EXPECT_TRUE(shape.glyphs.empty());
    Logger::Log::Flush();
    const auto messages = ring->GetMessages(Logger::LogLevel::Warning, 0, "[Text] Font");
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_NE(messages[0].Message.find("fallback"), std::string::npos);
}

TEST(FontAtlasShapingTests, CoverageProbeRequiresALoadedFaceAndUsesFallbacks)
{
    FontAtlas atlas;
    EXPECT_TRUE(atlas.RendersAllGlyphs(""));
    EXPECT_FALSE(atlas.RendersAllGlyphs("Play"));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(LoadFontBytes(TestFontPath("Roboto-Regular.ttf").string(), bytes));
    ASSERT_TRUE(atlas.LoadFontBytes(bytes.data(), bytes.size(), 48));
    EXPECT_TRUE(atlas.RendersAllGlyphs("Play 3 coins\n"));
    EXPECT_FALSE(atlas.RendersAllGlyphs("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));
    // Fallbacks are fixed before loading a primary face, like other atlas configuration.
    FontAtlas withFallback;
    ASSERT_TRUE(withFallback.SetFallbackFonts({TestFontPath("NotoSansHebrew-Regular.ttf")}));
    ASSERT_TRUE(withFallback.LoadFontBytes(bytes.data(), bytes.size(), 48));
    EXPECT_TRUE(withFallback.RendersAllGlyphs("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));
}
