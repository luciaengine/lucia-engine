// The box-shadow paint of the UI SDF shader: the same scene gives the same bytes on every frame.
#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace GameEngine;

namespace
{

// Each card's shadow rect has integer edges with an even sum of right and top
// (card x + 104, 26). Its inner ridge then runs through pixel centres, where some
// lanes of a 2x2 quad see a distance field with zero gradient.
constexpr const char* kCardsXml = R"(<uielement id='root'>
  <uielement class='card' /><uielement class='card' /><uielement class='card' /><uielement class='card' />
</uielement>)";

constexpr const char* kCardsCss = R"(
#root { display: flex; flex-direction: row; width: 800px; height: 600px; background-color: #1c1d1d; }
.card { width: 100px; height: 60px; margin: 20px; background-color: #335577; box-shadow: 4px 6px 8px rgba(0, 0, 0, 0.6); }
)";

constexpr int kFrames = 6;

size_t CountDifferingPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    constexpr size_t kPixelBytes = 8; // RGBA16F
    size_t differing = 0;
    for (size_t offset = 0; offset + kPixelBytes <= a.size(); offset += kPixelBytes)
        differing += std::memcmp(a.data() + offset, b.data() + offset, kPixelBytes) != 0 ? 1 : 0;
    return differing;
}

} // namespace

TEST(UIBoxShadowRender, AShadowRidgeOnPixelCentresGivesTheSameBytesEveryFrame)
{
    UITesting::IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kCardsXml, kCardsCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const UI::UITargetSpace space = UI::UITargetSpace::LinearSdr();
    const Rendering::ViewReadbackResult first = UITesting::RenderUiToReadback(fx.Manager(), space);
    ASSERT_FALSE(first.pixels.empty());
    for (int frame = 1; frame < kFrames; ++frame)
    {
        const Rendering::ViewReadbackResult next = UITesting::RenderUiToReadback(fx.Manager(), space);
        ASSERT_EQ(next.pixels.size(), first.pixels.size());
        EXPECT_EQ(CountDifferingPixels(first.pixels, next.pixels), 0u) << "frame " << frame;
    }
}
