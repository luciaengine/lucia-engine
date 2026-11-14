#include "EditorChromeFixture.h"
#include "PlayMode/PlayButtonState.h"
#include "PlayMode/PlayModeManager.h"

using namespace GameEngine;
using namespace EditorChromeTesting;

TEST(TopToolbarIcons, OutlineArtworkUses24PixelsWithoutChangingHitAreas)
{
    EditorChromeHost host(false);
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    auto owned = std::make_unique<UIElement>();
    auto* toolbar = owned.get();
    toolbar->SetId("ToolbarUnderTest");
    toolbar->AddClass("editor-top-toolbar");
    host.Ui->GetRootElement()->AddChild(std::move(owned));
    ASSERT_TRUE(host.Ui->InstantiateLayoutChildrenFromAssetPath(toolbar, "editor:UI/controls/EditorTopToolbar.uxml"));
    ASSERT_TRUE(host.Ui->AttachStyleToSubtreeFromAssetPath(toolbar, "editor:UI/controls/EditorTopToolbar.css"));
    auto pose = std::make_shared<Stylesheet>();
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        "#ToolbarUnderTest { width: 1440px; height: 40px; }", *pose));
    host.Ui->AddStylesheet(pose);
    host.Settle();
    const char* outlineIds[] = {
        "TopToolbarLeft1",
        "TopToolbarLeft2",
        "TopToolbarPackages",
        "TopToolbarLeft3",
        "TopToolbarTodos",
        "TopToolbarBookmarks",
        "TopToolbarBuild",
        "TopToolbarSettings",
        "TopToolbarHelp",
        "TopToolbarUniversalSearch",
        "TopToolbarCenter1",
        "TopToolbarUndoHistory",
        "TopToolbarRight1",
        "TopToolbarNodeGraph",
        "TopToolbarMixer",
        "TopToolbarAnimation",
        "TopToolbarTimeline",
        "TopToolbarVisualProfiler",
        "TopToolbarCpuProfiler",
        "TopToolbarMonitors",
        "TopToolbarVram",
        "TopToolbarRenderGraph",
        "TopToolbarLog",
        "TopToolbarRight2",
        "TopToolbarPanelMenu",
    };
    nlohmann::json geometry = nlohmann::json::array();
    // Calibration pose: larger hit areas must not scale the authored artwork.
    // Apply this same fixture-only32pxsize to both arms of the capture pair.
    for (const char* id : outlineIds)
    {
        auto* button = toolbar->FindById(id);
        ASSERT_NE(button, nullptr);
        button->Style().Set(Style::Width, StyleLength::Px(32));
        button->Style().Set(Style::Height, StyleLength::Px(32));
    }
    host.Settle();
    for (const char* id : outlineIds)
    {
        auto* button = dynamic_cast<Button*>(toolbar->FindById(id));
        ASSERT_NE(button, nullptr) << id;
        const auto& image = button->GetResolvedStyle().Visual.BackgroundImage;
        EXPECT_EQ(image.SizeMode, BackgroundSizeMode::Explicit) << id;
        EXPECT_FLOAT_EQ(image.SizeX, 24.0f) << id;
        EXPECT_FLOAT_EQ(image.SizeY, 24.0f) << id;
        EXPECT_FALSE(image.SizeXIsPercent) << id;
        EXPECT_FALSE(image.SizeYIsPercent) << id;
        EXPECT_TRUE(image.Source.HasImage()) << id;
        const auto rect = button->GetWindowRect();
        EXPECT_FLOAT_EQ(rect.Width, 32.0f) << id;
        EXPECT_FLOAT_EQ(rect.Height, 32.0f) << id;
        geometry.push_back({{"id", id}, {"x", rect.X}, {"y", rect.Y}, {"w", rect.Width}, {"h", rect.Height}});
    }
    for (const char* id : {"TopToolbarCenter2", "TopToolbarCenter3", "TopToolbarCenter4", "TopToolbarRecord", "TopToolbarTilePalette"})
    {
        auto* excluded = toolbar->FindById(id);
        ASSERT_NE(excluded, nullptr);
        EXPECT_FALSE(excluded->HasClass("toolbar-outline-icon")) << id;
    }
    if (const char* directory = std::getenv("GE_TOP_TOOLBAR_CAPTURE_DIR"))
    {
        std::filesystem::create_directories(directory);
        std::ofstream(std::filesystem::path(directory) / "all-outline-button-geometry.json") << geometry.dump(2);
        // Isolate the real left group; ids make the pose override the owning
        // subtree sheet without changing any production class or selector.
        ClassIn(toolbar, "top-toolbar-center")->SetId("CaptureToolbarCenter");
        ClassIn(toolbar, "top-toolbar-right")->SetId("CaptureToolbarRight");
        auto capture = std::make_shared<Stylesheet>();
        ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
            "#ToolbarUnderTest { width: 800px; }"
            "#CaptureToolbarCenter { display: none; }"
            "#CaptureToolbarRight { display: none; }", *capture));
        host.Ui->AddStylesheet(capture);
        host.Settle();
        const auto bytes = UITesting::RenderUiToBytes(*host.Ui, UI::UITargetSpace::EncodedSrgb());
        ASSERT_EQ(bytes.size(), size_t{800 * 600 * 4});
        size_t lit = 0;
        for (size_t p = 0; p < bytes.size(); p += 4)
            if (bytes[p] || bytes[p+1] || bytes[p+2]) ++lit;
        ASSERT_GT(lit, 1000u);
        std::ofstream image(std::filesystem::path(directory) / "left-toolbar.ppm", std::ios::binary);
        image << "P6\n800 600\n255\n";
        for (size_t p = 0; p < bytes.size(); p += 4)
            image.write(reinterpret_cast<const char*>(bytes.data() + p), 3);
        ASSERT_TRUE(image);
    }
}

TEST(TopToolbarIcons, PlayingTintSurvivesAClickAndClearsWhenPlayStops)
{
    EditorChromeHost host(false);
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    auto owned = std::make_unique<UIElement>();
    auto* toolbar = owned.get();
    toolbar->SetId("PlayToolbarUnderTest");
    toolbar->AddClass("editor-top-toolbar");
    host.Ui->GetRootElement()->AddChild(std::move(owned));
    ASSERT_TRUE(host.Ui->InstantiateLayoutChildrenFromAssetPath(toolbar, "editor:UI/controls/EditorTopToolbar.uxml"));
    ASSERT_TRUE(host.Ui->AttachStyleToSubtreeFromAssetPath(toolbar, "editor:UI/controls/EditorTopToolbar.css"));
    auto pose = std::make_shared<Stylesheet>();
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        "#PlayToolbarUnderTest { width: 1440px; height: 40px; }", *pose));
    host.Ui->AddStylesheet(pose);
    host.Settle();
    auto* play = dynamic_cast<Button*>(toolbar->FindById("TopToolbarCenter4"_sid));
    ASSERT_NE(play, nullptr);
    ASSERT_TRUE(play->GetResolvedStyle().Visual.BackgroundImage.HasTint);
    const auto restingTint = play->GetResolvedStyle().Visual.BackgroundImage.Tint;

    Editor::ApplyPlayButtonState(*play, Editor::PlayModeState::Play);
    host.Settle();
    ASSERT_TRUE(play->GetResolvedStyle().Visual.BackgroundImage.HasTint);
    const auto playingTint = play->GetResolvedStyle().Visual.BackgroundImage.Tint;
    EXPECT_NE(playingTint, restingTint);
    ASSERT_GT(play->GetLayoutWidth(), 0.0f);
    ASSERT_GT(play->GetLayoutHeight(), 0.0f);
    int clicks = 0;
    play->RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });
    UIEvent down{};
    down.Id = kEventMouseDown;
    down.Button = 0;
    down.Target = down.CurrentTarget = play;
    down.X = play->GetLayoutX() + play->GetLayoutWidth() * 0.5f;
    down.Y = play->GetLayoutY() + play->GetLayoutHeight() * 0.5f;
    play->OnEvent(down);
    ASSERT_TRUE(play->HasClass("pressed"));
    UIEvent up{};
    up.Id = kEventMouseUp;
    up.Button = 0;
    up.Target = up.CurrentTarget = play;
    up.X = down.X;
    up.Y = down.Y;
    play->OnEvent(up);
    EXPECT_EQ(clicks, 1);
    EXPECT_FALSE(play->HasClass("pressed"));
    host.Settle();
    ASSERT_TRUE(play->GetResolvedStyle().Visual.BackgroundImage.HasTint);
    EXPECT_EQ(play->GetResolvedStyle().Visual.BackgroundImage.Tint, playingTint);

    Editor::ApplyPlayButtonState(*play, Editor::PlayModeState::Edit);
    host.Settle();
    ASSERT_TRUE(play->GetResolvedStyle().Visual.BackgroundImage.HasTint);
    EXPECT_EQ(play->GetResolvedStyle().Visual.BackgroundImage.Tint, restingTint);
}
