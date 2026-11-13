// A tooltip built from a layout (UIElement::SetTooltipTemplate): the template is bound and
// measured each time it shows, before its first visible frame, and the stylesheets of the
// source's document style the bubble, the template and the arrow.

#include <gtest/gtest.h>

#include "FixedScalePlatform.h"
#include "UIRgTestHarness.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace GameEngine;
using GameEngine::UITesting::FixedScalePlatform;

namespace
{

constexpr char kLayoutPath[] = "UI/CommandTooltip.uxml";
constexpr char kLayout[] = R"(<uielement class="command-tooltip">
  <label id="tip-title" class="tip-title"/>
  <label id="tip-cost" class="tip-cost"/>
</uielement>)";

// The document's own sheet, attached to its container as a game document's is: it styles
// the bubble and the template's parts.
constexpr char kDocumentCss[] = R"(
.tooltip { background-color: #2a1c10; border-width: 2px; border-color: #d4a020; border-radius: 6px; padding: 6px; }
.tip-title { color: #ffd060; }
.tip-cost { color: #c0c0c0; }
)";

constexpr char kTitle[] = "Barracks";
constexpr char kCost[] = "150 gold, 50 lumber, 60 s";
constexpr char kNeighborText[] = "Move";
constexpr float kFrameSeconds = 0.05f;
constexpr float kSourceLeft = 300.0f;
constexpr float kNeighborLeft = 20.0f;
// A bubble left of this x belongs to the neighbor; right of it, to the template's source.
constexpr float kSidesSplitX = 150.0f;

// A UIManager over an asset tree that holds one tooltip layout, with a 400 x 300 root that
// holds a document container and, in it, the template's source on the right and a source
// with a text tooltip on the left.
class TemplateTooltipFixture
{
public:
    ~TemplateTooltipFixture()
    {
        m_Rg.reset();
        m_Ui.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    bool Init(std::string& why)
    {
        m_Device = SharedHeadlessDevice();
        if (!m_Device)
        {
            why = "no Vulkan device";
            return false;
        }
        namespace fs = std::filesystem;
        m_Root = fs::temp_directory_path() / "ui_tooltip_template_tests";
        std::error_code ec;
        fs::remove_all(m_Root, ec);
        fs::create_directories(m_Root / "UI", ec);
        {
            std::ofstream layout(m_Root / kLayoutPath);
            layout << kLayout;
        }
        if (!m_Assets.Initialize(m_Root, &m_Pool) || !m_Assets.GetRegistry().RegisterAsset(m_Root / kLayoutPath))
        {
            why = "asset manager init failed";
            return false;
        }

        m_Platform = std::make_unique<FixedScalePlatform>(1.0f);
        m_Ui = std::make_unique<UIManager>(m_Device, &m_Assets);
        m_Ui->SetPlatform(m_Platform.get());
        m_Ui->SetLayoutSizeOverride(400, 300);

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        root->Style().Set(Style::Width, StyleLength::Px(400.0f)).Set(Style::Height, StyleLength::Px(300.0f));
        auto document = std::make_unique<UIElement>();
        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(kDocumentCss, sheet))
        {
            why = "document CSS did not parse";
            return false;
        }
        document->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));
        auto source = std::make_unique<UIElement>();
        source->SetId("source");
        source->Style()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionLeft, StyleLength::Px(kSourceLeft))
            .Set(Style::PositionTop, StyleLength::Px(40.0f))
            .Set(Style::Width, StyleLength::Px(60.0f))
            .Set(Style::Height, StyleLength::Px(30.0f));
        m_Source = source.get();
        document->AddChild(std::move(source));
        auto neighbor = std::make_unique<UIElement>();
        neighbor->SetTooltip(kNeighborText);
        neighbor->Style()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionLeft, StyleLength::Px(kNeighborLeft))
            .Set(Style::PositionTop, StyleLength::Px(40.0f))
            .Set(Style::Width, StyleLength::Px(60.0f))
            .Set(Style::Height, StyleLength::Px(30.0f));
        document->AddChild(std::move(neighbor));
        root->AddChild(std::move(document));
        m_Ui->SetRoot(std::move(root));
        m_Rg = std::make_unique<UiRgHarness>(m_Device);
        Step();
        return true;
    }

    void Step()
    {
        m_Ui->Update(kFrameSeconds, /*interactive=*/true);
        DriveUiRender(*m_Ui, *m_Rg);
    }

    UIManager& Manager() { return *m_Ui; }
    UIElement& Source() { return *m_Source; }
    UIElement* Find(const char* id) { return m_Ui->GetRootElement()->FindById(HashStringId(id)); }

private:
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    std::filesystem::path m_Root;
    JobSystem::WorkStealingThreadPool m_Pool{2};
    AssetManager m_Assets;
    std::unique_ptr<FixedScalePlatform> m_Platform;
    Rendering::IDevice* m_Device = nullptr;
    std::unique_ptr<UIManager> m_Ui;
    std::unique_ptr<UiRgHarness> m_Rg;
    UIElement* m_Source = nullptr;
};

void BindCommand(UIElement& tooltip)
{
    if (auto* title = dynamic_cast<Label*>(tooltip.FindById("tip-title"_sid)))
        title->SetText(kTitle);
    if (auto* cost = dynamic_cast<Label*>(tooltip.FindById("tip-cost"_sid)))
        cost->SetText(kCost);
}

// True when the bubble is placed in the window; a hidden bubble is parked off it.
bool BubbleShown(TemplateTooltipFixture& fixture)
{
    const UIElement* wrapper = fixture.Find("ui-tooltip-wrapper");
    return wrapper && wrapper->GetLayoutX() >= 0.0f && wrapper->GetLayoutY() >= 0.0f;
}

} // namespace

// Checks every frame on which the template's bubble shows: it has its measured size and the
// template's labels hold the binder's text. Returns the count of those frames.
int ExpectTemplateBubbleFramesMeasured(TemplateTooltipFixture& fixture, bool skipUntilParked)
{
    bool parked = !skipUntilParked;
    int shownFrames = 0;
    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.Step();
        const UIElement* wrapper = fixture.Find("ui-tooltip-wrapper");
        parked = parked || (wrapper && wrapper->GetLayoutX() < 0.0f);
        if (!parked || !BubbleShown(fixture) || wrapper->GetLayoutX() < kSidesSplitX)
            continue;
        ++shownFrames;
        const UIElement* bubble = fixture.Find("ui-tooltip");
        const auto* title = dynamic_cast<const Label*>(fixture.Find("tip-title"));
        const auto* cost = dynamic_cast<const Label*>(fixture.Find("tip-cost"));
        EXPECT_NE(bubble, nullptr);
        EXPECT_NE(title, nullptr) << "frame " << frame << " shows the bubble without the template";
        EXPECT_NE(cost, nullptr) << "frame " << frame << " shows the bubble without the template";
        if (!bubble || !title || !cost)
            return shownFrames;
        EXPECT_GT(bubble->GetLayoutWidth(), 0.0f) << "frame " << frame << " shows the bubble at size zero";
        EXPECT_GT(title->GetLayoutWidth(), 0.0f) << "frame " << frame << " shows the template unmeasured";
        EXPECT_EQ(title->GetTextContent(), kTitle) << "frame " << frame << " shows the template unbound";
        EXPECT_EQ(cost->GetTextContent(), kCost) << "frame " << frame << " shows the template unbound";
        EXPECT_GE(bubble->GetLayoutWidth(), cost->GetLayoutWidth()) << "frame " << frame << " shows the bubble at a stale size";
    }
    return shownFrames;
}

// The template shows from a cold hover, and again when the pointer moves to its source
// straight from a shown text tooltip, which skips the hover delay. On every frame it shows
// it is measured and bound; the binder runs once for each show.
TEST(TooltipTemplateTests, ATemplateTooltipMountsBindsAndMeasuresBeforeItsFirstVisibleFrame)
{
    TemplateTooltipFixture fixture;
    std::string why;
    if (!fixture.Init(why))
    {
        if (why == "no Vulkan device")
            GTEST_SKIP() << why;
        FAIL() << why;
    }

    int binds = 0;
    fixture.Source().SetTooltipTemplate(kLayoutPath, [&binds](UIElement& tooltip) {
        ++binds;
        BindCommand(tooltip);
    });
    fixture.Source().SetTooltipPlacement(UIElement::TooltipPlacement::Below);

    // A cold hover. The overlay creates its wrapper on this first hovered frame, unlaid and
    // empty, and parks it off the window while the template measures.
    fixture.Manager().OnMouseMove(kSourceLeft + 20.0f, 50.0f);
    EXPECT_GT(ExpectTemplateBubbleFramesMeasured(fixture, /*skipUntilParked=*/true), 0) << "the tooltip never showed";
    EXPECT_EQ(binds, 1) << "the binder runs once each time the tooltip shows";
    const UIElement* text = fixture.Find("ui-tooltip-text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None) << "the text label shows beside the template";

    // The neighbor's text tooltip shows, then the pointer moves straight to the template's source.
    fixture.Manager().OnMouseMove(kNeighborLeft + 20.0f, 50.0f);
    for (int frame = 0; frame < 20; ++frame)
        fixture.Step();
    ASSERT_TRUE(BubbleShown(fixture)) << "the neighbor's tooltip never showed";
    ASSERT_LT(fixture.Find("ui-tooltip-wrapper")->GetLayoutX(), kSidesSplitX);
    EXPECT_EQ(fixture.Find("tip-title"), nullptr) << "the text tooltip shows the template's parts";
    EXPECT_EQ(text->GetResolvedStyle().Layout.DisplayMode, DisplayMode::Flex) << "the text tooltip hides its label";
    fixture.Manager().OnMouseMove(kSourceLeft + 20.0f, 50.0f);
    EXPECT_GT(ExpectTemplateBubbleFramesMeasured(fixture, /*skipUntilParked=*/false), 0) << "the tooltip never showed again";
    EXPECT_EQ(binds, 2) << "the binder runs once each time the tooltip shows";
    const UIElement* bubble = fixture.Find("ui-tooltip");
    ASSERT_NE(bubble, nullptr);
    EXPECT_EQ(bubble->GetChildren().size(), 3u) << "the bubble holds the hidden label and one copy of the template";
}

// The tooltip hides, then shows again for the same element: the template mounts and binds again.
TEST(TooltipTemplateTests, TheBinderRunsAgainWhenTheTooltipShowsAgainForTheSameElement)
{
    TemplateTooltipFixture fixture;
    std::string why;
    if (!fixture.Init(why))
    {
        if (why == "no Vulkan device")
            GTEST_SKIP() << why;
        FAIL() << why;
    }

    int binds = 0;
    fixture.Source().SetTooltipTemplate(kLayoutPath, [&binds](UIElement& tooltip) {
        ++binds;
        BindCommand(tooltip);
    });
    fixture.Manager().OnMouseMove(kSourceLeft + 20.0f, 50.0f);
    EXPECT_GT(ExpectTemplateBubbleFramesMeasured(fixture, /*skipUntilParked=*/true), 0) << "the tooltip never showed";
    fixture.Manager().OnMouseMove(kSidesSplitX, 250.0f);
    for (int frame = 0; frame < 20; ++frame)
        fixture.Step();
    ASSERT_FALSE(BubbleShown(fixture)) << "the tooltip did not hide";
    fixture.Manager().OnMouseMove(kSourceLeft + 20.0f, 50.0f);
    EXPECT_GT(ExpectTemplateBubbleFramesMeasured(fixture, /*skipUntilParked=*/false), 0) << "the tooltip never showed again";
    EXPECT_EQ(binds, 2) << "the binder runs once each time the tooltip shows";
}

// A class that the binder adds to the bubble leaves with the template, as the class of the
// layout's root does: the next text tooltip does not take them.
TEST(TooltipTemplateTests, TheClassesTheTemplateAddsToTheBubbleLeaveWithIt)
{
    TemplateTooltipFixture fixture;
    std::string why;
    if (!fixture.Init(why))
    {
        if (why == "no Vulkan device")
            GTEST_SKIP() << why;
        FAIL() << why;
    }

    fixture.Source().SetTooltipTemplate(kLayoutPath, [](UIElement& tooltip) {
        BindCommand(tooltip);
        tooltip.AddClass("tip-urgent");
    });
    fixture.Manager().OnMouseMove(kSourceLeft + 20.0f, 50.0f);
    EXPECT_GT(ExpectTemplateBubbleFramesMeasured(fixture, /*skipUntilParked=*/true), 0) << "the tooltip never showed";
    const UIElement* bubble = fixture.Find("ui-tooltip");
    ASSERT_NE(bubble, nullptr);
    ASSERT_TRUE(bubble->HasClass("tip-urgent")) << "the binder's class is on the bubble while the template shows";
    ASSERT_TRUE(bubble->HasClass("command-tooltip")) << "the layout root's class is on the bubble";

    fixture.Manager().OnMouseMove(kNeighborLeft + 20.0f, 50.0f);
    for (int frame = 0; frame < 20; ++frame)
        fixture.Step();
    ASSERT_TRUE(BubbleShown(fixture)) << "the neighbor's tooltip never showed";
    ASSERT_LT(fixture.Find("ui-tooltip-wrapper")->GetLayoutX(), kSidesSplitX);
    EXPECT_FALSE(bubble->HasClass("tip-urgent")) << "the binder's class stays on the next tooltip";
    EXPECT_FALSE(bubble->HasClass("command-tooltip")) << "the layout root's class stays on the next tooltip";
}

// The document's sheet reaches the overlay: the template's parts and the bubble take its
// rules, and the arrow takes the bubble's fill and border.
TEST(TooltipTemplateTests, TheBubbleAndItsTemplateTakeTheSourceDocumentsStylesheet)
{
    TemplateTooltipFixture fixture;
    std::string why;
    if (!fixture.Init(why))
    {
        if (why == "no Vulkan device")
            GTEST_SKIP() << why;
        FAIL() << why;
    }

    fixture.Source().SetTooltipTemplate(kLayoutPath, BindCommand);
    fixture.Source().SetTooltipPlacement(UIElement::TooltipPlacement::Below);
    fixture.Manager().OnMouseMove(kSourceLeft + 20.0f, 50.0f);
    for (int frame = 0; frame < 20; ++frame)
        fixture.Step();
    ASSERT_TRUE(BubbleShown(fixture)) << "the tooltip never showed";

    const UIElement* title = fixture.Find("tip-title");
    const UIElement* bubble = fixture.Find("ui-tooltip");
    ASSERT_NE(title, nullptr);
    ASSERT_NE(bubble, nullptr);
    EXPECT_EQ(title->GetResolvedStyle().Visual.Color, 0xFFFFD060u) << "the document's .tip-title rule";
    EXPECT_EQ(bubble->GetResolvedStyle().Visual.BackgroundColor, 0xFF2A1C10u) << "the document's .tooltip rule";

    const UI::UIPrimitive* border = fixture.Manager().PeekTooltipArrowForTesting(0);
    const UI::UIPrimitive* fill = fixture.Manager().PeekTooltipArrowForTesting(1);
    ASSERT_NE(border, nullptr) << "no arrow emitted";
    ASSERT_NE(fill, nullptr) << "a bordered bubble's arrow is a border triangle and a fill triangle";
    EXPECT_EQ(border->FillColor, UI::PackFromARGB(0xFFD4A020u)) << "the arrow takes the document's border";
    EXPECT_EQ(fill->FillColor, UI::PackFromARGB(0xFF2A1C10u)) << "the arrow takes the document's background";
}
