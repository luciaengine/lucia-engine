#include <gtest/gtest.h>
#include <string>

#include "UI/Parsers/CSSParser.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

// Parser coverage for the CSS viewport units (vw, vh, vmin, vmax). The sizing
// and inset properties keep the unit for layout to resolve. Every case cascades
// a plain 40px first, so "rejected" is observable as the cascaded value standing.
namespace
{

constexpr float kCascadedPx = 40.0f;

ResolvedStyle ResolveRule(const std::string& property, const std::string& value)
{
    Stylesheet sheet{};
    const std::string css =
        ".t { " + property + ": 40px; } .t { " + property + ": " + value + "; }";
    EXPECT_TRUE(CSSParser::ParseStylesFromString(css, sheet)) << css;
    UIElement el;
    el.AddClass("t");
    const ElementState state{};
    return CSSParser::ComputeStyleFor(el, sheet, state);
}

void ExpectLength(const StyleLength& actual, StyleLength::UnitType unit, float value,
                  const std::string& what)
{
    EXPECT_EQ(actual.Unit, unit) << what;
    EXPECT_FLOAT_EQ(actual.Value, value) << what;
}

} // namespace

TEST(CSSViewportUnits, EachUnitParsesWithAndWithoutDecimals)
{
    using Unit = StyleLength::UnitType;
    ExpectLength(ResolveRule("width", "50vw").Layout.Width, Unit::Vw, 50.0f, "50vw");
    ExpectLength(ResolveRule("width", "12.5vw").Layout.Width, Unit::Vw, 12.5f, "12.5vw");
    ExpectLength(ResolveRule("height", "100vh").Layout.Height, Unit::Vh, 100.0f, "100vh");
    ExpectLength(ResolveRule("height", "33.3vh").Layout.Height, Unit::Vh, 33.3f, "33.3vh");
    ExpectLength(ResolveRule("min-width", "10vmin").Layout.MinWidth, Unit::Vmin, 10.0f, "10vmin");
    ExpectLength(ResolveRule("min-height", "0.5vmin").Layout.MinHeight, Unit::Vmin, 0.5f, "0.5vmin");
    ExpectLength(ResolveRule("max-height", "20vmax").Layout.MaxHeight, Unit::Vmax, 20.0f, "20vmax");
    ExpectLength(ResolveRule("flex-basis", "7.25vmax").Layout.FlexBasis, Unit::Vmax, 7.25f, "7.25vmax");
    // Units are ASCII case-insensitive, as every CSS unit is.
    ExpectLength(ResolveRule("width", "50VW").Layout.Width, Unit::Vw, 50.0f, "50VW");
}

TEST(CSSViewportUnits, TheSafeBoxMaxWidthParses)
{
    ExpectLength(ResolveRule("max-width", "233.33vh").Layout.MaxWidth, StyleLength::UnitType::Vh,
                 233.33f, "max-width: 233.33vh");
}

TEST(CSSViewportUnits, InsetsTakeNegativeViewportLengths)
{
    const ResolvedStyle rs = ResolveRule("left", "-5vw");
    ExpectLength(rs.Layout.PositionLeft, StyleLength::UnitType::Vw, -5.0f, "left: -5vw");
    ExpectLength(ResolveRule("bottom", "2vh").Layout.PositionBottom, StyleLength::UnitType::Vh, 2.0f,
                 "bottom: 2vh");
}

TEST(CSSViewportUnits, ANegativeSizeIsRejectedInEveryUnit)
{
    ExpectLength(ResolveRule("width", "-10vw").Layout.Width, StyleLength::UnitType::Px, kCascadedPx,
                 "width: -10vw");
    ExpectLength(ResolveRule("max-width", "-1vh").Layout.MaxWidth, StyleLength::UnitType::Px,
                 kCascadedPx, "max-width: -1vh");
    ExpectLength(ResolveRule("min-height", "-3px").Layout.MinHeight, StyleLength::UnitType::Px,
                 kCascadedPx, "min-height: -3px");
    ExpectLength(ResolveRule("flex-basis", "-5%").Layout.FlexBasis, StyleLength::UnitType::Px,
                 kCascadedPx, "flex-basis: -5%");
    // A math function that folds below zero clamps to zero.
    ExpectLength(ResolveRule("width", "calc(10vw - 20vw)").Layout.Width, StyleLength::UnitType::Vw, 0.0f,
                 "width: calc(10vw - 20vw)");
}

TEST(CSSViewportUnits, AMalformedViewportLengthIsRejected)
{
    ExpectLength(ResolveRule("width", "vh").Layout.Width, StyleLength::UnitType::Px, kCascadedPx, "vh");
    ExpectLength(ResolveRule("width", "10 vh").Layout.Width, StyleLength::UnitType::Px, kCascadedPx,
                 "10 vh");
    ExpectLength(ResolveRule("width", "1.2.3vw").Layout.Width, StyleLength::UnitType::Px, kCascadedPx,
                 "1.2.3vw");
    // A number a float cannot hold, a hex number and infinity are not CSS numbers.
    ExpectLength(ResolveRule("width", "1e300vw").Layout.Width, StyleLength::UnitType::Px, kCascadedPx,
                 "1e300vw");
    ExpectLength(ResolveRule("flex", "1 1 0x10vw").Layout.FlexBasis, StyleLength::UnitType::Px,
                 kCascadedPx, "flex: 1 1 0x10vw");
    ExpectLength(ResolveRule("flex", "1 1 infvw").Layout.FlexBasis, StyleLength::UnitType::Px,
                 kCascadedPx, "flex: 1 1 infvw");
}

TEST(CSSViewportUnits, APropertyThatCannotResolveThemRejectsThem)
{
    // A margin and a padding do not read a viewport length as px.
    const ResolvedStyle margin = ResolveRule("margin-left", "10vh");
    EXPECT_FLOAT_EQ(margin.Layout.Margin.Left, kCascadedPx);
    EXPECT_FALSE(margin.Layout.MarginIsPercent.Left);
    const ResolvedStyle padding = ResolveRule("padding-top", "5vw");
    EXPECT_FLOAT_EQ(padding.Layout.Padding.Top, kCascadedPx);
    // A px-only property ignores the declaration; it does not read the number as 0.
    EXPECT_FLOAT_EQ(ResolveRule("border-top-width", "1vw").Layout.BorderWidth.Top, kCascadedPx);
    EXPECT_FLOAT_EQ(ResolveRule("flex-grow", "3vw").Layout.FlexGrow, kCascadedPx);
    EXPECT_FLOAT_EQ(ResolveRule("line-height", "2vh").Visual.LineHeight, kCascadedPx);
    EXPECT_FLOAT_EQ(ResolveRule("letter-spacing", "1vw").Visual.LetterSpacing, kCascadedPx);
    // Opacity clamps the cascaded 40px to 1.
    EXPECT_FLOAT_EQ(ResolveRule("opacity", "1vw").Visual.Opacity, 1.0f);
}

TEST(CSSViewportUnits, MathFunctionsFoldOneViewportUnit)
{
    ExpectLength(ResolveRule("max-width", "calc(116.665vh * 2)").Layout.MaxWidth,
                 StyleLength::UnitType::Vh, 233.33f, "calc(116.665vh * 2)");
    ExpectLength(ResolveRule("width", "max(10vw, 20vw)").Layout.Width, StyleLength::UnitType::Vw,
                 20.0f, "max(10vw, 20vw)");
    // Two units need a layout-time calc; the declaration is rejected whole.
    ExpectLength(ResolveRule("width", "calc(10vw + 10vh)").Layout.Width, StyleLength::UnitType::Px,
                 kCascadedPx, "calc(10vw + 10vh)");
    ExpectLength(ResolveRule("width", "min(320px, 70vw)").Layout.Width, StyleLength::UnitType::Px,
                 kCascadedPx, "min(320px, 70vw)");
}
