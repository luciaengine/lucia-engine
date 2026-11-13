#include "IsolatedUIFixture.h"

#include "UI/Parsers/CSSParser.h"
#include "UI/StyleDeclaration.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <memory>

// A write through UIElement::Style() must reach the element's own dirty bits, not only the
// declaration's record of them. Only UIElement::MarkDirty raises the parent's SubtreeDirty, so
// these tests read it to tell the two apart.

namespace GameEngine
{
namespace
{
struct ParentAndChild
{
    UIElement Parent;
    UIElement* Child = nullptr;

    ParentAndChild()
    {
        auto child = std::make_unique<UIElement>();
        Child = child.get();
        Parent.AddChild(std::move(child));
        Parent.ClearDirty(~0u);
        Child->ClearDirty(~0u);
    }
};
} // namespace

TEST(ElementStyleTests, LayoutPropertyMarksLayoutDirtyAndPropagates)
{
    ParentAndChild tree;

    tree.Child->Style().Set(Style::Width, StyleLength::Px(120.0f));

    EXPECT_TRUE(tree.Child->IsDirty(UIElement::LayoutDirty));
    EXPECT_TRUE(tree.Parent.IsDirty(UIElement::SubtreeDirty));
    ASSERT_TRUE(tree.Child->Style().Get(Style::Width).has_value());
    EXPECT_EQ(*tree.Child->Style().Get(Style::Width), StyleLength::Px(120.0f));
}

TEST(ElementStyleTests, PaintPropertyMarksVisualDirtyOnly)
{
    ParentAndChild tree;

    tree.Child->Style().Set(Style::BackgroundColor, 0xFF202020u);

    EXPECT_TRUE(tree.Child->IsDirty(UIElement::VisualDirty));
    EXPECT_FALSE(tree.Child->IsDirty(UIElement::LayoutDirty));
    EXPECT_FALSE(tree.Parent.IsDirty(UIElement::SubtreeDirty));
}

TEST(ElementStyleTests, SettingTheHeldValueMarksNothing)
{
    ParentAndChild tree;
    tree.Child->Style().Set(Style::Width, StyleLength::Px(120.0f));
    tree.Parent.ClearDirty(~0u);
    tree.Child->ClearDirty(~0u);

    tree.Child->Style().Set(Style::Width, StyleLength::Px(120.0f));

    EXPECT_FALSE(tree.Child->IsDirty(UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::StyleDirty));
    EXPECT_FALSE(tree.Parent.IsDirty(UIElement::SubtreeDirty));
}

TEST(ElementStyleTests, ResetRestylesAndPropagates)
{
    ParentAndChild tree;
    tree.Child->Style().Set(Style::BackgroundColor, 0xFF202020u);
    tree.Parent.ClearDirty(~0u);
    tree.Child->ClearDirty(~0u);

    tree.Child->Style().Reset(Style::BackgroundColor);

    EXPECT_FALSE(tree.Child->Style().Get(Style::BackgroundColor).has_value());
    EXPECT_TRUE(tree.Child->IsDirty(UIElement::StyleDirty));
    EXPECT_TRUE(tree.Parent.IsDirty(UIElement::SubtreeDirty));
}

TEST(ElementStyleTests, CodeStyleAndInlineStyleAreSeparateLayers)
{
    UIElement element;

    element.Style().Set(Style::Opacity, 0.5f);

    EXPECT_TRUE(element.Style().Get(Style::Opacity).has_value());
    EXPECT_FALSE(element.InlineStyle().Get(Style::Opacity).has_value());
    EXPECT_TRUE(element.InlineStyle().IsEmpty());
}

// The named background values are the ones the CSS spelling of the same declaration parses to.
TEST(ElementStyleTests, NamedBackgroundValuesMatchTheirCss)
{
    StyleDeclaration parsed;
    UIParsing::CSSParser::ParseInlineStyleToOverrides(
        "background-image: engine(\"scene_main\"); background-position: 25% 75%", parsed);

    const auto image = parsed.Get(Style::BackgroundImage);
    const auto position = parsed.Get(Style::BackgroundPosition);
    ASSERT_TRUE(image.has_value());
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(*image, BackgroundImageSource::Resource("scene_main"));
    EXPECT_EQ(*position, BackgroundPositionValue::Percent(25.0f, 75.0f));
}

// Style() wins over the `style` attribute: after the settle frames, an element with both resolves to
// the Style() value, and an element with the attribute alone resolves to the attribute's value, so
// the attribute does reach the cascade.
TEST(ElementStyleTests, CodeStyleWinsOverStyleAttribute)
{
    UITesting::IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f,
                                "<uielement id=\"root\">"
                                "<uielement id=\"both\" class=\"box\" style=\"opacity: 0.25\"/>"
                                "<uielement id=\"attribute\" class=\"box\" style=\"opacity: 0.25\"/>"
                                "</uielement>",
                                ".box { width: 10px; height: 10px; }");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();
    UIElement* both = fx.Element("both");
    UIElement* attribute = fx.Element("attribute");
    ASSERT_NE(both, nullptr);
    ASSERT_NE(attribute, nullptr);

    both->Style().Set(Style::Opacity, 0.5f);
    fx.Settle();

    EXPECT_FLOAT_EQ(attribute->GetResolvedStyle().Visual.LocalOpacity, 0.25f);
    EXPECT_FLOAT_EQ(both->GetResolvedStyle().Visual.LocalOpacity, 0.5f);
}

} // namespace GameEngine
