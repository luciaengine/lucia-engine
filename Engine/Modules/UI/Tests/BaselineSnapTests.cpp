// Text baselines must land on whole device pixels.
//
// WHAT THIS FIXES: the bottom terminals of stems and of round letters sit ON
// the baseline, so a fractional baseline spreads them across two rows — and
// spreads them DIFFERENTLY as the run moves, because the sub-pixel offset
// changes with the element's Y. Snapping removes that dependence, which buys
// vertical stability across content scale and scroll position as much as it
// buys crispness.
//
// WHAT THIS DOES NOT FIX: interior horizontal features. The staged Roboto face
// reports sCapHeight 1456 over a 2048 upem, so cap height at 13px is 9.2422px:
// even from an exactly integral baseline the capital-T crossbar lands 9.24px up
// and straddles two rows. Putting THAT on a
// whole row needs outline grid-fitting (FreeType TARGET_LIGHT/NORMAL over a
// scaled load), which this change does not attempt. The snap is a PREREQUISITE
// for that work, not a substitute: a standalone FreeType experiment found that
// at a +0.25px baseline offset a HINTED glyph's coverage profile is byte-for-
// byte the UNHINTED one, so hinting is a measured no-op until the origin is
// integral.
//
// It also does not fix underlines, because this engine has none: no
// `text-decoration` entry exists among the 133 properties in
// CSSValueParsers.cpp's table, so the three theme rules that declare it are
// parsed and dropped, and nothing anywhere emits an underline primitive.
//
// HOW THE BASELINE IS RECOVERED: the emitted primitive is the only thing these
// tests read, and it carries the quad top, not the baseline. MakeSlugGlyph
// stores the undilated em-space bounds in UvRect, and H = (EmYMax - EmYMin) *
// emScale (FontAtlas.cpp:458), so inverting the quad-top formula
// `gy = baseline - EmYMax * emScale` (FontAtlas.cpp:456) gives
//     emScale  = H / (UvRect[1] - UvRect[3])
//     baseline = Y + UvRect[1] * emScale
// which is a pure readback of emitted geometry — no expectation is copied from
// the code under test.
//
// Do not "correct" the UvRect indices against MakeSlugGlyph's parameter names:
// its 6th/8th parameters are named emYMin/emYMax, but the Y flip means callers
// pass gp.v0 = EmYMax into the one named emYMin (FontAtlas.cpp:469-471). The
// slots really do hold UvRect[1] = EmYMax, UvRect[3] = EmYMin.

#include "IsolatedUIFixture.h"

#include "UI/GlyphRunEmitter.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kLabelXml[] = R"(<uielement id="root">
  <label id="text">Hamburgefonstiv</label>
</uielement>)";

// A deliberately odd height and padding, so the baseline is fractional before
// content scale is even applied: measured unsnapped, this label needs a real
// correction at scale 1.0 (22.5), 1.25 (28.375) and 1.5 (33.75), and none at
// 2.0. Three discriminating cases and one control.
constexpr char kLabelCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 7px; }
#text { font-family: Roboto; font-size: 13px; height: 21px; color: #ffffff; }
)";

constexpr char kTwoLineXml[] = R"(<uielement id="root">
  <label id="text">Hamburge
fonstiv</label>
</uielement>)";

constexpr char kTwoLineCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 7px; }
#text { font-family: Roboto; font-size: 13px; height: 60px; line-height: 27px; color: #ffffff; }
)";

// Baseline recovered from one emitted Slug primitive. See the file header.
float RecoveredBaseline(const GameEngine::UI::UIPrimitive& p)
{
    const float emSpan = p.UvRect[1] - p.UvRect[3];
    const float emScale = p.H / emSpan;
    return p.Y + p.UvRect[1] * emScale;
}

std::vector<GameEngine::UI::UIPrimitive> Glyphs(const IsolatedUIFixture& fx)
{
    return fx.Primitives("text", GameEngine::UI::PrimitiveMode::Slug);
}

// Glyphs whose em span is degenerate carry no usable baseline (whitespace never
// reaches emission, but guard rather than divide by ~0).
bool HasUsableEmSpan(const GameEngine::UI::UIPrimitive& p)
{
    return (p.UvRect[1] - p.UvRect[3]) > 1e-4f && p.H > 1e-4f;
}

} // namespace

// The headline property, at every content scale the engine ships.
TEST(BaselineSnap, BaselineIsIntegralAtEveryContentScale)
{
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f})
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, kLabelXml, kLabelCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();
        // Expected numbers came from Roboto's outlines; a silent fallback face
        // would make every reading below meaningless.
        ASSERT_NE(fx.ResolvedFontFamily("text").find("Roboto"), std::string::npos)
            << "scale " << scale << ": resolved to " << fx.ResolvedFontFamily("text");

        const auto glyphs = Glyphs(fx);
        ASSERT_FALSE(glyphs.empty()) << "no glyphs emitted at scale " << scale;

        for (size_t i = 0; i < glyphs.size(); ++i)
        {
            if (!HasUsableEmSpan(glyphs[i]))
                continue;
            const float baseline = RecoveredBaseline(glyphs[i]);
            EXPECT_NEAR(baseline, std::round(baseline), 0.01f)
                << "scale " << scale << ", glyph " << i
                << ": baseline " << baseline << " is not on a whole device pixel";
        }
    }
}

// Catches the per-glyph-rounding mistake — and ONLY that mistake.
//
// Rounding (originY + gp.y) per glyph would round each glyph's own QUAD TOP,
// and since every glyph has a different top, each absorbs a different
// correction. Measured by injecting exactly that into EmitGlyphRun: one line's
// recovered baselines scattered over 28.0 … 28.609375 at content scale 1.25,
// and this test went red. A rigid per-line shift moves every glyph by ONE
// delta, so all recovered baselines on a line stay in agreement.
//
// What it CANNOT catch is the defect this change actually fixes. With no snap
// at all, every glyph on a line still shares one baseline — a fractional one —
// so this test is GREEN in the unsnapped build (measured). It is a guard on the
// SHAPE of the correction, not evidence that a correction happens; that is
// BaselineIsIntegralAtEveryContentScale's job, and only its.
TEST(BaselineSnap, PerLineShiftIsRigid)
{
    for (float scale : {1.25f, 1.5f})
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, kLabelXml, kLabelCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();
        ASSERT_NE(fx.ResolvedFontFamily("text").find("Roboto"), std::string::npos)
            << "scale " << scale << ": resolved to " << fx.ResolvedFontFamily("text");

        const auto glyphs = Glyphs(fx);
        ASSERT_GE(glyphs.size(), 2u) << "need several glyphs to compare, scale " << scale;

        bool haveRef = false;
        float reference = 0.0f;
        size_t refIndex = 0;
        for (size_t i = 0; i < glyphs.size(); ++i)
        {
            if (!HasUsableEmSpan(glyphs[i]))
                continue;
            const float baseline = RecoveredBaseline(glyphs[i]);
            if (!haveRef)
            {
                reference = baseline;
                refIndex = i;
                haveRef = true;
                continue;
            }
            EXPECT_NEAR(baseline, reference, 0.01f)
                << "scale " << scale << ": glyph " << i << " sits on baseline " << baseline
                << " but glyph " << refIndex << " sits on " << reference
                << " — the line was not shifted rigidly (per-glyph rounding?)";
        }
        ASSERT_TRUE(haveRef) << "no usable glyph at scale " << scale;
    }
}

// Multiple lines each get their own snap, and each stays internally rigid.
TEST(BaselineSnap, EachLineSnapsIndependentlyAndStaysRigid)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.25f, kTwoLineXml, kTwoLineCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    ASSERT_NE(fx.ResolvedFontFamily("text").find("Roboto"), std::string::npos)
        << "resolved to " << fx.ResolvedFontFamily("text");

    const auto glyphs = Glyphs(fx);
    ASSERT_GE(glyphs.size(), 4u);

    // Group recovered baselines; a two-line label must show exactly two.
    std::vector<float> distinct;
    for (const auto& g : glyphs)
    {
        if (!HasUsableEmSpan(g))
            continue;
        const float baseline = RecoveredBaseline(g);
        EXPECT_NEAR(baseline, std::round(baseline), 0.01f)
            << "baseline " << baseline << " is not on a whole device pixel";
        bool seen = false;
        for (float d : distinct)
            if (std::fabs(d - baseline) < 0.01f)
                seen = true;
        if (!seen)
            distinct.push_back(baseline);
    }
    EXPECT_EQ(distinct.size(), 2u)
        << "expected exactly one baseline per line; glyphs within a line must agree";
}

// The no-op control, at the unit level: a snap must not move a run that is
// already aligned, and must never move one by more than half a pixel.
//
// The integration-level control is BaselineIsIntegralAtEveryContentScale's
// scale-2.0 case — measured as the only one of the four whose baseline is
// already whole. Scale 1.0 is NOT a control: unsnapped, this label's baseline
// lands at 22.5 there, the largest error of any scale tested (1.25 → 28.375,
// 1.5 → 33.75). Fractional content scale is not what makes baselines
// fractional; a 21px line box around a 13px font is enough on its own.
TEST(BaselineSnap, SnapIsIdentityWhenAlreadyAlignedAndNeverExceedsHalfAPixel)
{
    using GameEngine::UI::SnapRunOriginY;

    // Already integral: bit-exact identity, not merely "close".
    EXPECT_EQ(SnapRunOriginY(10.0f, 12.0f), 10.0f);
    EXPECT_EQ(SnapRunOriginY(0.0f, 0.0f), 0.0f);
    EXPECT_EQ(SnapRunOriginY(-4.0f, 9.0f), -4.0f);

    // A fractional origin moves by exactly the amount that lands the baseline.
    EXPECT_NEAR(SnapRunOriginY(10.25f, 12.0f), 10.0f, 1e-6f);
    EXPECT_NEAR(SnapRunOriginY(10.75f, 12.0f), 11.0f, 1e-6f);

    // Bounded shift, over a spread of fractional origins and baselines.
    for (int i = 0; i < 100; ++i)
    {
        const float originY = static_cast<float>(i) * 0.37f - 18.0f;
        const float baselineY = static_cast<float>(i % 7) + 9.0f;
        const float snapped = SnapRunOriginY(originY, baselineY);
        EXPECT_LE(std::fabs(snapped - originY), 0.5f + 1e-5f)
            << "originY " << originY << " moved too far";
        const float baseline = snapped + baselineY;
        EXPECT_NEAR(baseline, std::round(baseline), 1e-4f);
    }
}

// Glyph pens land on quarter device pixels along x, as Chrome places glyphs on
// macOS (four subpixel phases per pixel), and the move is rigid: the quad keeps
// its offset from the pen, so the outline is untouched. Two glyphs whose pens
// sit at 3.10 and 3.15, either side of the 3.125 tie, go to 3.0 and 3.25; colour glyphs
// and glyphs without an em span keep their position.
TEST(GlyphPenSnap, PenXRoundsToTheNearestQuarterDevicePixel)
{
    std::array<GameEngine::Rendering::Text::FontAtlas::GlyphPlacement, 3> glyphs{};
    for (auto& glyph : glyphs)
    {
        // 10 px per em: the quad starts 1 px right of the pen.
        glyph.u0 = 0.1f;
        glyph.u1 = 0.6f;
        glyph.width = 5.0f;
        glyph.height = 8.0f;
        glyph.hBandCount = glyph.vBandCount = 1;
        glyph.color = 0xffffffffu;
    }
    glyphs[0].x = 1.0f - 0.02f; // pen at 3.100
    glyphs[1].x = 1.0f + 0.03f; // pen at 3.150
    glyphs[2].x = 1.0f + 0.03f;
    glyphs[2].u1 = glyphs[2].u0; // no em span: position kept

    std::vector<GameEngine::UI::UIPrimitive> primitives;
    GameEngine::UI::GlyphRunTarget target;
    target.Primitives = &primitives;
    GameEngine::UI::EmitGlyphRun(glyphs, 3.12f, 0.0f, 16.0f, target);
    ASSERT_EQ(primitives.size(), 3u);
    EXPECT_NEAR(primitives[0].X, 1.0f + 3.0f, 1e-4f);
    EXPECT_NEAR(primitives[1].X, 1.0f + 3.25f, 1e-4f);
    EXPECT_NEAR(primitives[2].X, 3.12f + 1.03f, 1e-4f);
}

namespace
{
constexpr char kPenXml[] = R"(<uielement id="root">
  <label id="lbl" text="Hamburgefonstiv"/>
  <textarea id="area" value="Hamburgefonstiv"/>
</uielement>)";

// A fractional left margin, so the layout pens are off the quarter grid at
// every content scale before the emitter rounds them.
constexpr char kPenCss[] = R"(
#root { display: flex; flex-direction: column; width: 600px; height: 400px; padding: 3.3px; }
#lbl { width: 300px; height: 40px; font-family: Roboto; font-size: 16px; margin-left: 7.3px; }
#area { width: 300px; height: 40px; font-family: Roboto; font-size: 16px; margin-left: 7.3px; padding: 0px; }
)";
} // namespace

// The quarter-pixel pen rounding happens in device pixels on every path that
// emits glyph runs, labels (UIManager_PrimitiveGen) and text areas
// (TextArea.cpp, EmitGlyphRun): the emitted quads are in render pixels, so at
// content scale 2 a pen lands on a quarter of a device pixel, an eighth of a
// logical one. The pen is recovered from the quad as X - EmXMin * emScale.
TEST(GlyphPenSnap, PensLandOnQuarterDevicePixelsInLabelsAndTextAreas)
{
    for (float scale : {1.0f, 1.25f, 2.0f})
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, kPenXml, kPenCss);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();
        for (const char* id : {"lbl", "area"})
        {
            const auto glyphs = fx.Primitives(id, GameEngine::UI::PrimitiveMode::Slug);
            ASSERT_GE(glyphs.size(), 10u) << id << " at scale " << scale;
            for (size_t i = 0; i < glyphs.size(); ++i)
            {
                const GameEngine::UI::UIPrimitive& p = glyphs[i];
                const float emWidth = p.UvRect[2] - p.UvRect[0];
                if (emWidth <= 1e-4f)
                    continue;
                const float pen = p.X - p.UvRect[0] * (p.W / emWidth);
                EXPECT_NEAR(pen * 4.0f, std::round(pen * 4.0f), 0.01f)
                    << id << " glyph " << i << " at scale " << scale << ": pen " << pen;
            }
        }
    }
}
