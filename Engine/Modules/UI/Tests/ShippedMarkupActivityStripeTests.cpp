#include "Core/ApplicationPaths.h"
// The Mark-ups panel's Activity stripes, as the editor ships them: every second row sits on the
// theme's secondary list stripe, an opaque plate the first row does not draw. Read from the staged
// sheets.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::PathUtils;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

std::string ReadShippedCss(const std::string& relative)
{
    std::ifstream in(ApplicationPaths::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="list" class="markups-list">
    <uielement id="row" class="markups-row markups-activity-row"/>
    <uielement id="alt" class="markups-row markups-activity-row markups-activity-row-alt"/>
  </uielement>
</uielement>)";

constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 200px; height: 200px; }
)";

} // namespace

TEST(ShippedMarkupActivityStripeTests, TheSecondRowSitsOnTheSecondaryStripe)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "panels/MarkupsPanel.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kGeometryCss;

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    for (const char* id : {"row", "alt"})
        ASSERT_NE(fx.Style(id), nullptr) << id;

    const uint32_t row = fx.Style("row")->Visual.BackgroundColor;
    const uint32_t alt = fx.Style("alt")->Visual.BackgroundColor;

    EXPECT_EQ(alt >> 24, 0xFFu) << "the second row is on an opaque plate";
    EXPECT_NE(alt, row) << "the second row differs from the first";
}

TEST(ShippedMarkupActivityStripeTests, AThemeCanUseTheSamePlateForBothRows)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "panels/MarkupsPanel.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text + '\n';
    }
    css += kGeometryCss;
    css += R"(
:root {
    --ui_color_list_stripe_primary: #123456;
    --ui_color_list_stripe_secondary: #123456;
}
#swatch { background-color: #123456; width: 10px; height: 10px; }
)";
    constexpr char xml[] = R"(<uielement id="root">
      <uielement id="row" class="markups-row markups-activity-row"/>
      <uielement id="alt" class="markups-row markups-activity-row markups-activity-row-alt"/>
      <uielement id="swatch"/>
    </uielement>)";
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, xml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    for (const char* id : {"row", "alt", "swatch"})
        ASSERT_NE(fx.Style(id), nullptr) << id;
    const uint32_t swatch = fx.Style("swatch")->Visual.BackgroundColor;
    EXPECT_EQ(swatch >> 24, 0xFFu);
    EXPECT_EQ(fx.Style("row")->Visual.BackgroundColor, swatch);
    EXPECT_EQ(fx.Style("alt")->Visual.BackgroundColor, swatch);
}

TEST(ShippedMarkupActivityStripeTests, LogFilterFillUsesTheThemeOverride)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "panels/LogPanel.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text + '\n';
    }
    css += kGeometryCss;
    css += R"(
:root { --ui_color_log_error_fill: #123456; }
#swatch { background-color: #123456; width: 10px; height: 10px; }
)";
    constexpr char xml[] = R"(<uielement id="root">
      <uielement id="filter" class="log-filter-toggle log-filter-level-error checked"/>
      <uielement id="swatch"/>
    </uielement>)";
    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, xml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    ASSERT_NE(fx.Style("filter"), nullptr);
    ASSERT_NE(fx.Style("swatch"), nullptr);
    EXPECT_EQ(fx.Style("filter")->Visual.BackgroundColor, fx.Style("swatch")->Visual.BackgroundColor);
}
