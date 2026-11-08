#include "EditorChromeFixture.h"
#include "Panels/LogPanel/LogChannelPopover.h"

using namespace GameEngine;
using namespace EditorChromeTesting;

TEST(LogPanelActions, AuthoredButtonsTogglePauseCopyAndClear)
{
    EditorChromeHost host;
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    auto* pause = host.Action("log-pause-button");
    auto* copy = host.Action("log-copy-button");
    auto* clear = host.Action("log-clear-button");
    ASSERT_NE(pause, nullptr); ASSERT_NE(copy, nullptr); ASSERT_NE(clear, nullptr);
    EXPECT_NE(ClassIn(pause, "log-action-pause-icon"), nullptr);
    EXPECT_LT(pause->GetLayoutX(), copy->GetLayoutX());
    EXPECT_LT(copy->GetLayoutX(), clear->GetLayoutX());
    Click(pause);
    EXPECT_TRUE(host.Panel->GetLogView()->IsPaused());
    EXPECT_EQ(pause->GetText(), "Resume");
    EXPECT_NE(ClassIn(pause, "log-action-resume-icon"), nullptr);
    Click(pause);
    EXPECT_FALSE(host.Panel->GetLogView()->IsPaused());
    EXPECT_EQ(pause->GetText(), "Pause");
    EXPECT_NE(ClassIn(pause, "log-action-pause-icon"), nullptr);
    Logger::LogMessage message{};
    message.Level = Logger::LogLevel::Info;
    message.Message = "Log toolbar regression fixture";
    host.Panel->GetLogView()->AddMessage(message);
    host.Panel->Update();
    Click(pause);
    Click(copy);
    EXPECT_NE(host.Platform.GetClipboardText().find(message.Message), std::string::npos);
    Click(clear);
    EXPECT_EQ(host.Panel->GetLogView()->GetMessageCount(), 0u);
    host.Settle();
    if (const char* directory = std::getenv("GE_LOG_TOOLBAR_CAPTURE_DIR"))
    {
        nlohmann::json nodes = nlohmann::json::array();
        std::function<void(UIElement*)> inspect = [&](UIElement* el) {
            const auto rect = el->GetWindowRect();
            const auto& style = el->GetResolvedStyle();
            nodes.push_back({{"classes", el->GetClasses()}, {"x", rect.X}, {"y", rect.Y},
                {"w", rect.Width}, {"h", rect.Height}, {"display", int(style.Layout.DisplayMode)},
                {"opacity", style.Visual.Opacity}, {"image", style.Visual.BackgroundImage.Source.Value}});
            for (const auto& child : el->GetChildren()) inspect(child.get());
        };
        auto* search = ClassIn(host.Panel, "panel-search-bar");
        ASSERT_NE(search, nullptr);
        inspect(search);
        std::filesystem::create_directories(directory);
        std::ofstream(std::filesystem::path(directory) / "search-layout.json") << nodes.dump(2);
        EXPECT_GE(search->GetLayoutWidth(), 88.0f);
        const auto bytes = UITesting::RenderUiToBytes(*host.Ui, UI::UITargetSpace::EncodedSrgb());
        ASSERT_EQ(bytes.size(), size_t{800 * 600 * 4});
        size_t colored = 0;
        for (size_t p = 0; p < bytes.size(); p += 4)
            if (bytes[p] || bytes[p+1] || bytes[p+2]) ++colored;
        ASSERT_GT(colored, 1000u);
        std::filesystem::create_directories(directory);
        std::ofstream image(std::filesystem::path(directory) / "log-compact-paused.ppm", std::ios::binary);
        image << "P6\n800 600\n255\n";
        for (size_t p = 0; p < bytes.size(); p += 4)
            image.write(reinterpret_cast<const char*>(bytes.data() + p), 3);
        ASSERT_TRUE(image);
    }
}

TEST(LogPanelActions, ResponsiveReparentingRestoresSearchAndActionsTogether)
{
    EditorChromeHost host;
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    auto* center = ClassIn(host.Panel, "log-toolbar-center-slot");
    auto* search = ClassIn(host.Panel, "log-toolbar-search");
    auto* primary = ClassIn(host.Panel, "log-toolbar-primary");
    auto* secondary = ClassIn(host.Panel, "log-toolbar-secondary");
    ASSERT_NE(center, nullptr); ASSERT_NE(search, nullptr);
    ASSERT_NE(primary, nullptr); ASSERT_NE(secondary, nullptr);
    EXPECT_GE(search->GetLayoutWidth(), 88.0f);
    EXPECT_LE(search->GetWindowRect().Right(), host.Panel->GetWindowRect().Right());
    auto* searchIcon = ClassIn(search, "panel-search-icon");
    ASSERT_NE(searchIcon, nullptr);
    EXPECT_GT(searchIcon->GetLayoutWidth(), 0.0f);
    EXPECT_LE(searchIcon->GetWindowRect().Right(), search->GetWindowRect().Right());
    EXPECT_EQ(center->GetParent(), secondary); EXPECT_EQ(search->GetParent(), secondary);
    EXPECT_TRUE(search->HasClass("log-toolbar-search-compact"));
    host.Panel->Overrides().Set(Style::Width, StyleLength::Px(1200)); host.Settle();
    EXPECT_EQ(center->GetParent(), primary); EXPECT_EQ(search->GetParent(), primary);
    EXPECT_TRUE(secondary->HasClass("log-toolbar-secondary-hidden"));
    EXPECT_FALSE(search->HasClass("log-toolbar-search-compact"));
    host.Panel->Overrides().Set(Style::Width, StyleLength::Px(600)); host.Settle();
    EXPECT_EQ(center->GetParent(), secondary); EXPECT_EQ(search->GetParent(), secondary);
    // Reproduce the half-applied transition the private repair phase owns.
    auto moved = secondary->TakeChild(search); ASSERT_NE(moved, nullptr);
    primary->AddChild(std::move(moved)); host.Settle();
    EXPECT_EQ(center->GetParent(), search->GetParent());
    EXPECT_EQ(search->HasClass("log-toolbar-search-compact"), search->GetParent() == secondary);
}

TEST(LogPanelActions, ChannelPickerHidesAndRestoresRows)
{
    EditorChromeHost host;
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);

    Logger::LogMessage message(Logger::LogLevel::Info, "Channel picker regression fixture");
    message.Channel = Logger::RegisterChannel("Editor.ChannelPickerRegression", Logger::LogLevel::Trace).Index;
    auto* view = host.Panel->GetLogView();
    view->AddMessage(message);
    host.Panel->Update();

    auto* channels = dynamic_cast<Button*>(host.Panel->FindById("LogChannels"));
    ASSERT_NE(channels, nullptr);
    Click(channels);
    host.Settle();
    auto* hideAll = dynamic_cast<Button*>(host.Panel->FindById("LogChannelsHideAll"));
    auto* showAll = dynamic_cast<Button*>(host.Panel->FindById("LogChannelsShowAll"));
    ASSERT_NE(hideAll, nullptr);
    ASSERT_NE(showAll, nullptr);

    Click(hideAll);
    EXPECT_EQ(view->GetAllMessagesAsText(true).find(message.Message), std::string::npos);
    EXPECT_NE(view->GetAllMessagesAsText(false).find(message.Message), std::string::npos);
    Click(showAll);
    EXPECT_NE(view->GetAllMessagesAsText(true).find(message.Message), std::string::npos);
}

TEST(LogPanelActions, ChannelPickerClosesWhenItsAnchorIsRemoved)
{
    EditorChromeHost host;
    if (!host.Device)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    auto* channels = dynamic_cast<LogChannelPopover*>(ClassIn(host.Panel, "log-channels-anchor"));
    ASSERT_NE(channels, nullptr);
    {
        UIElement anchor;
        channels->Open(anchor);
        ASSERT_TRUE(channels->IsPopupOpen());
    }
    host.Settle();
    EXPECT_FALSE(channels->IsPopupOpen());
}
