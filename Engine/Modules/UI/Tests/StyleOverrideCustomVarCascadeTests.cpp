#include "SharedHeadlessDevice.h"
#include "UI/Controls/Mount.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleDeclaration.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <gtest/gtest.h>

#include <memory>

namespace GameEngine {

// An override set on the parent after the first frame reaches the child's var()
// on the next frame. Set, change, ClearCustom and Clear() each cascade the child again.
TEST(StyleOverrideCustomVarCascadeTests, ParentOverrideReachesChildVarLength)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";

    Stylesheet sheet{};
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        ".probe-child { width: calc(10px * var(--probe_scale, 1)); height: 4px; }\n", sheet));

    auto root = std::make_unique<UIElement>();
    root->Style()
        .Set(Style::Width, StyleLength::Px(200.0f))
        .Set(Style::Height, StyleLength::Px(200.0f))
        .Set(Style::AlignItems, AlignItems::FlexStart);
    auto child = std::make_unique<UIElement>();
    child->AddClass("probe-child");
    UIElement* parentRaw = root.get();
    UIElement* childRaw = child.get();
    root->AddChild(std::move(child));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    ASSERT_FLOAT_EQ(childRaw->GetLayoutWidth(), 10.0f) << "the var() fallback applies before any override";

    const StringId scale = HashStringId("--probe_scale");
    parentRaw->Style().SetCustomNumber(scale, 1.7f);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_FLOAT_EQ(childRaw->GetLayoutWidth(), 17.0f) << "the parent's override did not reach the child's var()";

    parentRaw->Style().SetCustomNumber(scale, 2.5f);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_FLOAT_EQ(childRaw->GetLayoutWidth(), 25.0f) << "a changed override did not re-resolve the child";

    parentRaw->Style().ClearCustom(scale);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_FLOAT_EQ(childRaw->GetLayoutWidth(), 10.0f) << "a cleared override did not restore the fallback";

    parentRaw->Style().SetCustomNumber(scale, 3.0f);
    ui.Update(0.0f, /*interactive=*/false);
    ASSERT_FLOAT_EQ(childRaw->GetLayoutWidth(), 30.0f);
    parentRaw->Style().Clear();
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_FLOAT_EQ(childRaw->GetLayoutWidth(), 10.0f) << "clearing every override did not restore the fallback";
}

// A Mount's portal target cascades with the Mount as its parent, so a custom
// property set on an ancestor of the Mount reaches the target's var() too.
TEST(StyleOverrideCustomVarCascadeTests, AncestorOverrideReachesMountTargetVarLength)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No device";
    UIRegistration::RegisterBuiltInControls();

    Stylesheet sheet{};
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        ".probe-child { width: calc(10px * var(--probe_scale, 1)); height: 4px; }\n", sheet));

    auto root = std::make_unique<UIElement>();
    root->Style()
        .Set(Style::Width, StyleLength::Px(200.0f))
        .Set(Style::Height, StyleLength::Px(200.0f))
        .Set(Style::AlignItems, AlignItems::FlexStart);
    auto mount = std::make_unique<Mount>();
    mount->Style().Set(Style::AlignItems, AlignItems::FlexStart);
    Mount* mountRaw = mount.get();
    UIElement* rootRaw = root.get();
    root->AddChild(std::move(mount));

    // The target is owned outside the tree; the Mount only borrows it.
    auto target = std::make_unique<UIElement>();
    target->AddClass("probe-child");

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));
    mountRaw->SetTarget(target.get());

    ui.Update(0.0f, /*interactive=*/false);
    ASSERT_FLOAT_EQ(target->GetLayoutWidth(), 10.0f) << "the var() fallback applies before any override";

    rootRaw->Style().SetCustomNumber(HashStringId("--probe_scale"), 2.0f);
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_FLOAT_EQ(target->GetLayoutWidth(), 20.0f) << "the ancestor's override did not reach the Mount target";

    mountRaw->SetTarget(nullptr);
}

} // namespace GameEngine
