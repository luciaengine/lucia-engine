#include "Panels/LogView.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using GameEngine::LogView;

namespace
{

Logger::LogMessage MakeMsg(const char* text, const char* file, const char* function)
{
    Logger::LogMessage msg;
    msg.Level = Logger::LogLevel::Info;
    msg.Message = text ? text : "";
    msg.Timestamp = std::chrono::system_clock::now();
    msg.SetSourceLocation(file, 42, function);
    return msg;
}

const LogView::StoredMessage* FlushOne(LogView& view)
{
    view.FlushPendingMessages();
    EXPECT_EQ(view.GetMessageCount(), 1u);
    return view.GetMessage(0);
}

} // namespace

TEST(LogViewTests, CopiesValidSourceAndFunction)
{
    LogView view;
    view.AddMessage(MakeMsg("hello", "Foo.cpp", "Bar"));
    const auto* stored = FlushOne(view);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->Text, "hello");
    EXPECT_EQ(stored->SourceFile, "Foo.cpp");
    EXPECT_EQ(stored->Function, "Bar");
    EXPECT_EQ(stored->SourceLine, 42);
}

TEST(LogViewTests, NullSourceAndFunctionBecomeEmpty)
{
    LogView view;
    view.AddMessage(MakeMsg("hello", nullptr, nullptr));
    const auto* stored = FlushOne(view);
    ASSERT_NE(stored, nullptr);
    EXPECT_TRUE(stored->SourceFile.empty());
    EXPECT_TRUE(stored->Function.empty());
}

TEST(LogViewTests, CombinesChannelSearchAndSeverityFiltersForStoredAndNewRows)
{
    LogView view;
    const auto channel = Logger::RegisterChannel("LogViewTests", Logger::LogLevel::Trace).Index;
    auto hidden = MakeMsg("needle hidden channel", "Hidden.cpp", "Write");
    hidden.Channel = channel;
    auto warning = MakeMsg("needle hidden severity", "Visible.cpp", "Write");
    warning.Level = Logger::LogLevel::Warning;

    view.AddMessage(hidden);
    view.AddMessage(warning);
    view.FlushPendingMessages();
    view.SetChannelsShown({&channel, 1}, false);
    view.SetWarningFilterEnabled(false);
    view.SetSearchFilter("needle");

    view.AddMessage(hidden);
    view.AddMessage(warning);
    view.AddMessage(MakeMsg("other visible channel", "Visible.cpp", "Write"));
    view.AddMessage(MakeMsg("needle visible match", "Visible.cpp", "Write"));
    view.FlushPendingMessages();

    auto text = view.GetAllMessagesAsText(true);
    EXPECT_NE(text.find("needle visible match"), std::string::npos);
    EXPECT_EQ(text.find("hidden"), std::string::npos);
    EXPECT_EQ(text.find("other"), std::string::npos);
    EXPECT_NE(view.GetAllMessagesAsText(false).find("needle hidden channel"), std::string::npos);

    view.SetChannelsShown({&channel, 1}, true);
    text = view.GetAllMessagesAsText(true);
    EXPECT_NE(text.find("needle hidden channel"), std::string::npos);
    EXPECT_EQ(text.find("needle hidden severity"), std::string::npos);
}
