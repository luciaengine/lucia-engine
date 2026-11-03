// The Mark-ups panel's view toggle, as the editor ships it: the chosen view's tab, a small
// secondary button marked markups-tab-chosen, fills with the accent blue and not with the secondary variant's
// own active color. Read from the staged sheets.

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
    std::ifstream in(PathUtils::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="chosen" class="button small secondary markups-tab markups-tab-chosen"/>
  <uielement id="secondaryActive" class="button small secondary active"/>
  <uielement id="accent" class="accent-swatch"/>
</uielement>)";

constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 200px; height: 100px; }
.accent-swatch { background-color: var(--ui_color_accent_blue); }
)";

} // namespace

TEST(ShippedMarkupsTabTests, TheChosenViewsTabFillsWithTheAccentBlue)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "theme/core.css", "controls/Button.css", "panels/MarkupsPanel.css"})
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
    ASSERT_NE(fx.Style("chosen"), nullptr);
    ASSERT_NE(fx.Style("secondaryActive"), nullptr);
    ASSERT_NE(fx.Style("accent"), nullptr);
    ASSERT_EQ(fx.Style("accent")->Visual.BackgroundColor >> 24, 0xFFu) << "--ui_color_accent_blue did not resolve";
    EXPECT_EQ(fx.Style("chosen")->Visual.BackgroundColor, fx.Style("accent")->Visual.BackgroundColor);
    EXPECT_NE(fx.Style("chosen")->Visual.BackgroundColor, fx.Style("secondaryActive")->Visual.BackgroundColor);
}
