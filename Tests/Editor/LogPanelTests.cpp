#include "EditorChromeFixture.h"
#include "Panels/LogPanel/LogChannelPopover.h"
#include "UI/Controls/TextField.h"

using namespace GameEngine;
using namespace EditorChromeTesting;

TEST(LogPanelActions, AuthoredButtonsTogglePauseCopyAndClear)
{
    EditorChromeHost host;
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    ASSERT_NE(host.View(), nullptr);
    auto* pause = host.Action("LogPause");
    auto* copy = host.Action("LogCopyAll");
    auto* clear = host.Action("LogClear");
    ASSERT_NE(pause, nullptr); ASSERT_NE(copy, nullptr); ASSERT_NE(clear, nullptr);
    EXPECT_LT(pause->GetLayoutX(), copy->GetLayoutX());
    EXPECT_LT(clear->GetLayoutX(), pause->GetLayoutX());
    Click(pause);
    EXPECT_TRUE(host.View()->IsPaused());
    EXPECT_EQ(pause->GetText(), "Resume");
    Click(pause);
    EXPECT_FALSE(host.View()->IsPaused());
    EXPECT_EQ(pause->GetText(), "Pause");
    Logger::LogMessage message{};
    message.Level = Logger::LogLevel::Info;
    message.Message = "Log toolbar regression fixture";
    host.View()->AddMessage(message);
    host.Panel->Update();
    Click(pause);
    Click(copy);
    EXPECT_NE(host.Platform.GetClipboardText().find(message.Message), std::string::npos);
    Click(clear);
    EXPECT_EQ(host.View()->GetModel().GetVisibleCount(), 0u);
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

TEST(LogPanelActions, ResponsiveWrappingKeepsSearchSeverityChannelsAndActionsReachable)
{
    EditorChromeHost host;
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    host.Ui->SetLayoutSizeOverride(1400, 600);
    host.Ui->GetRootElement()->Style().Set(Style::Width, StyleLength::Px(1400));
    auto* search = ClassIn(host.Panel, "log-search-bar");
    ASSERT_NE(search, nullptr);
    auto* searchIcon = ClassIn(search, "panel-search-icon");
    ASSERT_NE(searchIcon, nullptr);
    for (const float width : {600.0f, 1200.0f, 600.0f})
    {
        host.Panel->Style().Set(Style::Width, StyleLength::Px(width));
        host.Settle();
        const auto panelRect = host.Panel->GetWindowRect();
        for (const char* id : {"LogFilterError", "LogFilterWarning", "LogFilterInfo", "LogFilterDebug",
                               "LogChannels", "LogPause", "LogCopyAll", "LogClear", "log-search-field"})
        {
            const auto* control = host.Panel->FindById(HashStringId(id));
            ASSERT_NE(control, nullptr) << id;
            const auto rect = control->GetWindowRect();
            EXPECT_GT(rect.Width, 0.0f) << id;
            EXPECT_GT(rect.Height, 0.0f) << id;
            EXPECT_GE(rect.X, panelRect.X) << id;
            EXPECT_LE(rect.Right(), panelRect.Right() + 0.5f) << id;
            EXPECT_LE(rect.Bottom(), panelRect.Bottom() + 0.5f) << id;
        }
        EXPECT_GE(search->GetLayoutWidth(), 160.0f);
        EXPECT_GT(searchIcon->GetLayoutWidth(), 0.0f);
        EXPECT_LE(searchIcon->GetWindowRect().Right(), search->GetWindowRect().Right());
    }
}

TEST(LogPanelActions, ChannelPickerHidesAndRestoresRows)
{
    EditorChromeHost host;
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);

    Logger::LogMessage message(Logger::LogLevel::Info, "Channel picker regression fixture");
    message.Channel = Logger::RegisterChannel("Editor.ChannelPickerRegression", Logger::LogLevel::Trace).Index;
    auto* view = host.View();
    ASSERT_NE(view, nullptr);
    view->Clear();
    view->AddMessage(message);
    host.Panel->Update();
    view->SetSearch(message.Message);
    ASSERT_EQ(view->GetModel().GetVisibleCount(), 1u);
    const uint64_t sequence = view->GetModel().GetVisibleEntry(0)->Sequence;

    auto* channels = dynamic_cast<Button*>(host.Panel->FindById("LogChannels"_sid));
    ASSERT_NE(channels, nullptr);
    Click(channels);
    host.Settle();
    auto* hideAll = dynamic_cast<Button*>(host.Panel->FindById("LogChannelsHideAll"_sid));
    auto* showAll = dynamic_cast<Button*>(host.Panel->FindById("LogChannelsShowAll"_sid));
    ASSERT_NE(hideAll, nullptr);
    ASSERT_NE(showAll, nullptr);

    Click(hideAll);
    EXPECT_EQ(view->GetModel().FormatVisibleLines().find(message.Message), std::string::npos);
    ASSERT_NE(view->GetModel().FindEntry(sequence), nullptr);
    EXPECT_EQ(view->GetModel().FindEntry(sequence)->Text, message.Message);
    Click(showAll);
    EXPECT_NE(view->GetModel().FormatVisibleLines().find(message.Message), std::string::npos);
}

TEST(LogPanelActions, ChannelPickerPreservesSearchAndSeverityFilters)
{
    EditorChromeHost host;
    if (!host.Device) GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(host.Ready);
    auto* view = host.View();
    ASSERT_NE(view, nullptr);
    view->Clear();
    const auto channel = Logger::RegisterChannel("Editor.CombinedFilters", Logger::LogLevel::Trace).Index;
    for (const auto level : {Logger::LogLevel::Info, Logger::LogLevel::Error})
    {
        Logger::LogMessage message(level, "Combined filter witness");
        message.Channel = channel;
        view->AddMessage(message);
    }
    Logger::LogMessage unrelated(Logger::LogLevel::Error, "Unrelated witness");
    unrelated.Channel = channel;
    view->AddMessage(unrelated);
    host.Panel->Update();
    auto* search = dynamic_cast<TextField*>(host.Panel->FindById("log-search-field"_sid));
    ASSERT_NE(search, nullptr);
    search->SetValue("Combined filter");
    EXPECT_EQ(view->GetModel().GetVisibleCount(), 2u);
    auto* info = host.Action("LogFilterInfo");
    ASSERT_NE(info, nullptr);
    Click(info);
    EXPECT_EQ(view->GetModel().GetVisibleCount(), 1u);
    auto* channels = host.Action("LogChannels");
    ASSERT_NE(channels, nullptr);
    Click(channels);
    host.Settle();
    auto* hideAll = host.Action("LogChannelsHideAll");
    auto* showAll = host.Action("LogChannelsShowAll");
    ASSERT_NE(hideAll, nullptr);
    ASSERT_NE(showAll, nullptr);
    Click(hideAll);
    EXPECT_EQ(view->GetModel().GetVisibleCount(), 0u);
    Click(showAll);
    ASSERT_EQ(view->GetModel().GetVisibleCount(), 1u);
    EXPECT_EQ(view->GetModel().GetVisibleEntry(0)->Level, Logger::LogLevel::Error);
    EXPECT_EQ(search->GetValue(), "Combined filter");
    Click(info);
    EXPECT_EQ(view->GetModel().GetVisibleCount(), 2u);
    search->SetValue("witness");
    EXPECT_EQ(view->GetModel().GetVisibleCount(), 3u);
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
