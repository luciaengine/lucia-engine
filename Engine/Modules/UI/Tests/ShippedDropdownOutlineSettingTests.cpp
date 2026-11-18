// Settings > Controls > Dropdown Outlines, as the editor ships it: the setting stamps
// .dropdowns-no-outline on the UI root, and the shipped tokens.css and widgets.css
// decide the dropdown header's border color. A docked panel's tree is a Mount target
// with no parent, so it matches :root and declares every token again; the setting
// must still reach a dropdown inside that panel.

#include "SharedHeadlessDevice.h"

#include "Core/ApplicationPaths.h"
#include "UI/Controls/Mount.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

constexpr const char* kNoOutlineRootClass = "dropdowns-no-outline";
constexpr uint32_t kAlphaMask = 0xFF000000u;

std::string ReadShippedCss(const char* relative)
{
    std::ifstream in(ApplicationPaths::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The border color of a dropdown header in a docked panel under a root that does or
// does not carry the setting's class. Fails the test when a shipped sheet is missing.
uint32_t PanelDropdownBorderColor(bool rootHasNoOutlineClass)
{
    auto* dev = SharedHeadlessDevice();
    const std::string css = ReadShippedCss("theme/tokens.css") + ReadShippedCss("theme/widgets.css");
    Stylesheet sheet{};
    EXPECT_TRUE(UIParsing::CSSParser::ParseStylesFromString(css, sheet));

    auto root = std::make_unique<UIElement>();
    if (rootHasNoOutlineClass)
        root->AddClass(kNoOutlineRootClass);
    auto mount = std::make_unique<Mount>();
    Mount* mountRaw = mount.get();
    root->AddChild(std::move(mount));

    // The panel is owned outside the tree, as a dock owns its panels; the Mount only borrows it.
    auto panel = std::make_unique<UIElement>();
    auto header = std::make_unique<UIElement>();
    header->AddClass("dropdown-header");
    UIElement* headerRaw = header.get();
    panel->AddChild(std::move(header));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));
    mountRaw->SetTarget(panel.get());
    ui.Update(0.0f, /*interactive=*/false);

    const uint32_t color = headerRaw->GetResolvedStyle().Visual.BorderColor.Top;
    mountRaw->SetTarget(nullptr);
    return color;
}

} // namespace

TEST(ShippedDropdownOutlineSettingTests, ADockedPanelDropdownDrawsNoOutlineWhenTheSettingIsOff)
{
    if (!SharedHeadlessDevice())
        GTEST_SKIP() << "No Vulkan device available";
    UIRegistration::RegisterBuiltInControls();
    ASSERT_FALSE(ReadShippedCss("theme/tokens.css").empty()) << "theme/tokens.css is not staged next to the test";
    ASSERT_FALSE(ReadShippedCss("theme/widgets.css").empty()) << "theme/widgets.css is not staged next to the test";

    EXPECT_NE(PanelDropdownBorderColor(false) & kAlphaMask, 0u)
        << "with the setting on, the panel dropdown must draw its outline";
    EXPECT_EQ(PanelDropdownBorderColor(true) & kAlphaMask, 0u)
        << "with the setting off, the panel's :root tokens must not restore the outline";
}
