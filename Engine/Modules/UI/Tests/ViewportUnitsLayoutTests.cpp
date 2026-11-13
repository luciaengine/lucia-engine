// The CSS viewport units (vw, vh, vmin, vmax) resolve against the UI root's
// layout size: the host extent in logical px, after the UI scale. A resize or
// a UI scale change re-resolves them with no style change on the element.
//
// The safe box is the game HUD case: a frame that is at most 21:9 wide
// (233.33vh) and centered. Yoga ignores auto margins on an absolute box
// (MarginAutoTests.cpp, MarginAutoKnownGaps), so the box centers in a flex row.

#include "IsolatedUIFixture.h"

#include "UI/UIScaleSettings.h"

#include <algorithm>
#include <cstdint>

#include <gtest/gtest.h>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

// Yoga rounds onto a 1/64 px grid; a viewport length is not a whole pixel.
constexpr float kGridPx = 0.05f;

// 21:9 as a share of the viewport height: 7/3 of the height, in vh.
constexpr float kSafeBoxVh = 233.33f;

constexpr char kSafeBoxXml[] = R"(<uielement id="root">
  <uielement id="safe"/>
  <uielement id="probe"/>
</uielement>)";

constexpr char kSafeBoxCss[] = R"(
#root  { display: flex; flex-direction: row; justify-content: center; width: 100%; height: 100%; }
#safe  { width: 100%; height: 100%; max-width: 233.33vh; }
#probe { position: absolute; left: 1vw; top: 2vh; width: 10vmin; height: 10vmax; }
)";

constexpr char kResizeXml[] = R"(<uielement id="root"><uielement id="half"/></uielement>)";

constexpr char kResizeCss[] = R"(
#root { display: flex; flex-direction: column; align-items: flex-start; width: 100%; height: 100%; }
#half { width: 50vh; height: 10px; flex-shrink: 0; }
)";

struct SafeBoxCase
{
    uint32_t Width;
    uint32_t Height;
};

// Lays the safe box out at one host size and checks its width and centered margins.
void ExpectSafeBoxAt(IsolatedUIFixture& fx, const SafeBoxCase& size)
{
    fx.Manager().SetLayoutSizeOverride(size.Width, size.Height);
    fx.Settle();

    const float width = static_cast<float>(size.Width);
    const float height = static_cast<float>(size.Height);
    const float expectedWidth = std::min(width, kSafeBoxVh * height / 100.0f);
    const float expectedMargin = (width - expectedWidth) * 0.5f;

    const auto box = fx.BorderBox("safe");
    EXPECT_NEAR(box.W, expectedWidth, kGridPx) << size.Width << "x" << size.Height;
    EXPECT_NEAR(box.H, height, kGridPx) << size.Width << "x" << size.Height;
    EXPECT_NEAR(box.X, expectedMargin, kGridPx) << size.Width << "x" << size.Height;
    EXPECT_NEAR(width - (box.X + box.W), expectedMargin, kGridPx) << size.Width << "x" << size.Height;
}

} // namespace

TEST(ViewportUnitsLayout, SafeBoxIsAtMost21By9AndCenteredAtEachScreenShape)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kSafeBoxXml, kSafeBoxCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    // 16:9: 2520 px of 233.33vh caps at the 1920 px screen.
    ExpectSafeBoxAt(fx, {1920u, 1080u});
    // 2.39:1: 3360 px, 40 px in from each edge.
    ExpectSafeBoxAt(fx, {3440u, 1440u});
    // 32:9: 3360 px, 880 px in from each edge.
    ExpectSafeBoxAt(fx, {5120u, 1440u});
}

TEST(ViewportUnitsLayout, EachUnitResolvesAgainstItsViewportAxis)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kSafeBoxXml, kSafeBoxCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    fx.Manager().SetLayoutSizeOverride(3440u, 1440u);
    fx.Settle();

    const auto probe = fx.BorderBox("probe");
    EXPECT_NEAR(probe.X, 34.4f, kGridPx) << "left: 1vw of 3440";
    EXPECT_NEAR(probe.Y, 28.8f, kGridPx) << "top: 2vh of 1440";
    EXPECT_NEAR(probe.W, 144.0f, kGridPx) << "width: 10vmin of 1440";
    EXPECT_NEAR(probe.H, 344.0f, kGridPx) << "height: 10vmax of 3440";
}

TEST(ViewportUnitsLayout, AResizeReResolvesAViewportLength)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kResizeXml, kResizeCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    fx.Manager().SetLayoutSizeOverride(1920u, 1080u);
    fx.Settle();
    ASSERT_NEAR(fx.BorderBox("half").W, 540.0f, kGridPx);

    // The element's style does not change; only the host height does.
    fx.Manager().SetLayoutSizeOverride(1920u, 1440u);
    fx.Settle();
    EXPECT_NEAR(fx.BorderBox("half").W, 720.0f, kGridPx);
}

TEST(ViewportUnitsLayout, AUIScaleChangeReResolvesAgainstTheLogicalRoot)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kSafeBoxXml, kSafeBoxCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << fx.Diagnostic();
    ASSERT_TRUE(built) << fx.Diagnostic();

    GameEngine::UIManager& ui = fx.Manager();
    ui.SetLayoutSizeOverride(3440u, 1440u);
    fx.Settle();
    ASSERT_NEAR(ui.GetContentScale(), 1.0f, 1.0e-4f);
    ASSERT_NEAR(fx.BorderBox("safe").W, kSafeBoxVh * 14.4f, kGridPx);

    // Height mode at 1080p: scale 4/3, so the logical root is 2580x1080.
    ui.SetScaleSettings(GameEngine::UI::UIScaleSettings{GameEngine::UI::UIScaleMode::Height, 1920.0f, 1080.0f});
    fx.Settle();
    const float scale = ui.GetContentScale();
    ASSERT_NEAR(scale, 1440.0f / 1080.0f, 1.0e-4f);

    const GameEngine::UIElement* safe = fx.Element("safe");
    ASSERT_NE(safe, nullptr);
    // 233.33vh of the 1080 logical px root; the same 3360 physical px as before.
    EXPECT_NEAR(safe->GetLayoutWidth(), kSafeBoxVh * 10.8f, kGridPx);
    EXPECT_NEAR(safe->GetLayoutWidth() * scale, kSafeBoxVh * 14.4f, kGridPx * scale);
}
