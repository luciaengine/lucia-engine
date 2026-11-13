// A badge in a .badge-button (the prefab bar's pills), read from the staged sheets. The button sets the
// fill its badge reads: lighter with the button's hover class, darker while pressed. Teal override
// badges and amber warning badges each take their own theme tokens.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "Core/ApplicationPaths.h"

#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::ApplicationPaths;
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
  <button id="teal-button" class="badge-button">
    <label id="teal" class="inspector-section-header-badge inspector-section-header-badge-override" text="3 overrides"/>
  </button>
  <button id="amber-button" class="badge-button">
    <label id="amber" class="inspector-section-header-badge" text="1 not in prefab"/>
  </button>
  <uielement id="teal-rest" class="ref-teal-rest"/>
  <uielement id="teal-hover" class="ref-teal-hover"/>
  <uielement id="teal-pressed" class="ref-teal-pressed"/>
  <uielement id="amber-rest" class="ref-amber-rest"/>
  <uielement id="amber-hover" class="ref-amber-hover"/>
  <uielement id="amber-pressed" class="ref-amber-pressed"/>
</uielement>)";

constexpr char kTestCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 300px; height: 300px; }
.ref-teal-rest { background-color: var(--ui_color_prefab_background); }
.ref-teal-hover { background-color: var(--ui_color_prefab_background_hover); }
.ref-teal-pressed { background-color: var(--ui_color_prefab_background_pressed); }
.ref-amber-rest { background-color: var(--ui_color_warning_badge); }
.ref-amber-hover { background-color: var(--ui_color_warning_badge_hover); }
.ref-amber-pressed { background-color: var(--ui_color_warning_badge_pressed); }
)";

uint32_t Fill(const IsolatedUIFixture& fx, const std::string& id)
{
    const auto* style = fx.Style(id);
    return style ? style->Visual.BackgroundColor : 0u;
}

// The pointer over the button with its hover class set, as the button's owner sets it on pointer enter.
void HoverButton(IsolatedUIFixture& fx, const std::string& id)
{
    auto* element = fx.Element(id);
    ASSERT_NE(element, nullptr) << id;
    element->AddClass("hovered");
    fx.Manager().OnMouseMove(element->GetLayoutX() + element->GetLayoutWidth() * 0.5f,
                             element->GetLayoutY() + element->GetLayoutHeight() * 0.5f);
    fx.Settle();
}

} // namespace

TEST(ShippedBadgeButtonStateTests, BadgeInButtonGetsLighterWhenHoveredAndDarkerWhilePressed)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "controls/Button.css", "theme/inspector.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kTestCss;

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    EXPECT_EQ(Fill(fx, "teal"), Fill(fx, "teal-rest"));
    EXPECT_EQ(Fill(fx, "amber"), Fill(fx, "amber-rest"));

    HoverButton(fx, "teal-button");
    EXPECT_EQ(Fill(fx, "teal"), Fill(fx, "teal-hover"));
    EXPECT_EQ(Fill(fx, "teal-button") >> 24, 0u) << "the button stays transparent on hover";
    fx.Manager().OnMouseButton(0, true);
    fx.Settle();
    EXPECT_EQ(Fill(fx, "teal"), Fill(fx, "teal-pressed"));
    EXPECT_EQ(Fill(fx, "teal-button") >> 24, 0u) << "the button stays transparent while pressed";
    fx.Manager().OnMouseButton(0, false);
    fx.Settle();

    fx.Element("teal-button")->RemoveClass("hovered");
    HoverButton(fx, "amber-button");
    EXPECT_EQ(Fill(fx, "teal"), Fill(fx, "teal-rest"));
    EXPECT_EQ(Fill(fx, "amber"), Fill(fx, "amber-hover"));
    fx.Manager().OnMouseButton(0, true);
    fx.Settle();
    EXPECT_EQ(Fill(fx, "amber"), Fill(fx, "amber-pressed"));
    fx.Manager().OnMouseButton(0, false);
    fx.Settle();
}
