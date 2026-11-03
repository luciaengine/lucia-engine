// Every inspector colour chip is one control: `.inspector-color-swatch` in inspector.css owns its
// size, radius, border and cursor, and code sets only the colour. A chip that sizes or borders
// itself from code is a private copy of that style, and the copies drift.

#include <gtest/gtest.h>

#include "Inspectors/InspectorColorSwatchRow.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <optional>

using GameEngine::UIElement;
namespace Style = GameEngine::Style;

namespace
{
constexpr std::uint32_t kInitialArgb = 0xFF336699u;
constexpr std::uint32_t kPickedArgb = 0xFFCC8844u;
} // namespace

TEST(InspectorColorSwatchRow, TheRowAsksTheStylesheetForItsLook)
{
    UIElement root;
    const GameEngine::InspectorUI::ColorSwatchRow row = GameEngine::InspectorUI::AddColorSwatchRow(
        &root, "Tint", "A tooltip", kInitialArgb, "(0.20, 0.40, 0.60)");
    ASSERT_NE(row.Title, nullptr);
    ASSERT_NE(row.Field, nullptr);
    ASSERT_NE(row.Swatch, nullptr);
    ASSERT_NE(row.Value, nullptr);

    EXPECT_TRUE(row.Swatch->HasClass("inspector-color-swatch"));
    EXPECT_TRUE(row.Field->HasClass("inspector-color-swatch-field"));
    EXPECT_TRUE(row.Value->HasClass("inspector-color-swatch-value"));
    EXPECT_EQ(row.Value->GetText(), "(0.20, 0.40, 0.60)");
}

TEST(InspectorColorSwatchRow, CodeSetsOnlyTheChipColour)
{
    auto swatch = GameEngine::InspectorUI::MakeColorSwatch(kInitialArgb);
    ASSERT_NE(swatch, nullptr);
    EXPECT_EQ(swatch->Overrides().Get(Style::BackgroundColor), std::optional<std::uint32_t>(kInitialArgb));
    EXPECT_FALSE(swatch->Overrides().Get(Style::Width).has_value())
        << "the chip sizes itself from code instead of the stylesheet";
    EXPECT_FALSE(swatch->Overrides().Get(Style::BorderColor).has_value())
        << "the chip paints its border from code instead of the stylesheet";

    GameEngine::InspectorUI::SetColorSwatchColor(swatch.get(), kPickedArgb);
    EXPECT_EQ(swatch->Overrides().Get(Style::BackgroundColor), std::optional<std::uint32_t>(kPickedArgb));
}
