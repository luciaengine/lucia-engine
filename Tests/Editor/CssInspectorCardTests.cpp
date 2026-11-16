// The Ctrl+I hover card takes its look from CssInspector.css: labels and values in its two text
// colors, and a color chip before a color value. Over a real UIManager with the editor's sheets.

#include "CssInspector/CssInspector.h"
#include "InspectorLayoutFixture.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <memory>

using namespace GameEngine;

namespace
{
constexpr float kFrameSeconds = 1.0f / 60.0f;
// --ui_color_text_soft and --ui_color_text_bright in theme/tokens.css.
constexpr uint32_t kSoftText = 0xB3FFFFFFu;
constexpr uint32_t kBrightText = 0xFFFFFFFFu;

class CssInspectorCardTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto root = std::make_unique<UIElement>();
        auto target = std::make_unique<UIElement>();
        target->SetId("css-card-test-target");
        target->Style().Set(Style::BackgroundColor, 0xFF336699u);
        UI::Layout::SetAbsolutePosition(*target, Mathematics::Rect{200.0f, 200.0f, 120.0f, 40.0f});
        m_Target = target.get();
        root->AddChild(std::move(target));
        m_Fixture.ExtraSheets = {"Assets/UI/controls/CssInspector/CssInspector.css"};
        if (!m_Fixture.Build(std::move(root)))
            GTEST_SKIP() << m_Fixture.Diagnostic;
    }

    void Frame() { m_Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true); }

    // Hovers the target, lets the inspector build its card, and resolves the card's styles.
    UIElement* ShowCard()
    {
        m_Fixture.Ui->OnMouseMove(m_Target->GetLayoutX() + 10.0f, m_Target->GetLayoutY() + 10.0f);
        Frame();
        m_Inspector.SetEnabled(true);
        m_Inspector.Update(m_Fixture.Ui.get(), 800, 600);
        Frame();
        return m_Fixture.Ui->GetRootElement()->FindById(HashStringId("css-inspector-overlay"));
    }

    uint32_t TextColor(const UIElement* element) const
    {
        const ResolvedStyle* style = m_Fixture.Ui->TryGetResolvedStyleFor(element);
        return style ? style->Visual.Color : 0u;
    }

    InspectorLayoutTesting::InspectorLayoutFixture m_Fixture;
    CssInspector m_Inspector;
    UIElement* m_Target = nullptr;
};

TEST_F(CssInspectorCardTest, LabelsAndValuesTakeTheTwoTextColorsOfTheSheet)
{
    UIElement* card = ShowCard();
    ASSERT_NE(card, nullptr) << "hovering an element with the inspector on shows its card";
    EXPECT_TRUE(card->HasClass("css-inspector-card"));

    const UIElement* label = InspectorLayoutTesting::FindByClass(card, "css-inspector-label");
    const UIElement* value = InspectorLayoutTesting::FindByClass(card, "css-inspector-value");
    ASSERT_NE(label, nullptr);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(TextColor(label), kSoftText);
    EXPECT_EQ(TextColor(value), kBrightText);
}

TEST_F(CssInspectorCardTest, AColorValueGetsAChipFilledWithIt)
{
    UIElement* card = ShowCard();
    ASSERT_NE(card, nullptr);
    const UIElement* chip = InspectorLayoutTesting::FindByClass(card, "css-inspector-swatch");
    ASSERT_NE(chip, nullptr) << "the background line shows a color chip";
    const ResolvedStyle* style = m_Fixture.Ui->TryGetResolvedStyleFor(chip);
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->Visual.BackgroundColor, 0xFF336699u);
}
} // namespace
