#include <gtest/gtest.h>
#include "Logger/Logger.h"
#include "LoggerInternals.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace LoggerOwnershipTest
{
struct BorrowedValue
{
    const char* Text;
    int* Formatted;
};
struct ThrowingValue
{
};
} // namespace LoggerOwnershipTest

template <>
struct std::formatter<LoggerOwnershipTest::BorrowedValue> : std::formatter<std::string_view>
{
    auto format(const LoggerOwnershipTest::BorrowedValue& value, std::format_context& context) const
    {
        ++*value.Formatted;
        return std::formatter<std::string_view>::format(value.Text, context);
    }
};

template <>
struct std::formatter<LoggerOwnershipTest::ThrowingValue> : std::formatter<std::string_view>
{
    std::format_context::iterator format(const LoggerOwnershipTest::ThrowingValue&,
                                         std::format_context&) const
    {
        throw std::format_error("deliberate formatter failure");
    }
};

namespace
{
using namespace Logger;

class CaptureSink : public LogSink
{
  public:
    void Write(const LogMessage& message) override { Messages.push_back(message); }
    void Flush() override {}
    bool ShouldLog(LogLevel) const override { return true; }
    std::string GetName() const override { return "Capture"; }
    // Tests read only after Flush has acknowledged all writes.
    const std::vector<LogMessage>& GetMessages() const { return Messages; }
    size_t GetCount() const { return Messages.size(); }

  private:
    std::vector<LogMessage> Messages;
};

class LoggerOwnershipTestFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Initialize({});
        Log::ClearSinks();
        auto sink = std::make_unique<CaptureSink>();
        Spy = sink.get();
        Log::AddSink(std::move(sink));
        Log::Flush();
    }
    void TearDown() override { Log::Shutdown(); }
    CaptureSink* Spy = nullptr;
};

TEST_F(LoggerOwnershipTestFixture, FormatterAndBorrowedDataAreConsumedBeforeReturn)
{
    char text[] = "producer text";
    int formatted = 0;
    int line = 0;
    {
        std::unique_lock sinkLock(Log::GetStateForSharing()->Mutex);
        line = __LINE__ + 1;
        GE_LOG(CoreLog, Info, "value: {}", LoggerOwnershipTest::BorrowedValue{text, &formatted});
        EXPECT_EQ(formatted, 1);
        std::memset(text, 'x', sizeof(text) - 1);
    }
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, "value: producer text");
    EXPECT_EQ(messages[0].SourceFile, __FILE__);
    EXPECT_TRUE(messages[0].Function.ends_with("TestBody")) << messages[0].Function;
    EXPECT_EQ(messages[0].SourceLine, line);
    EXPECT_EQ(formatted, 1);
}

TEST_F(LoggerOwnershipTestFixture, FilteredMessagesDoNotInvokeUserFormatters)
{
    Log::SetLogLevel(LogLevel::Warning);
    int formatted = 0;
    Log::Info("{}", LoggerOwnershipTest::BorrowedValue{"ignored", &formatted});
    Log::Flush();
    EXPECT_EQ(formatted, 0);
    EXPECT_EQ(Spy->GetCount(), 0u);
}

TEST_F(LoggerOwnershipTestFixture, ChannelRecordKeepsItsSourceWhenFormattingFails)
{
    const int line = __LINE__ + 1;
    EXPECT_NO_THROW(GE_LOG(CoreLog, Info, "{}", LoggerOwnershipTest::ThrowingValue{}));
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, "[FORMAT ERROR: deliberate formatter failure]");
    EXPECT_EQ(messages[0].SourceFile, __FILE__);
    EXPECT_EQ(messages[0].SourceLine, line);
}

TEST_F(LoggerOwnershipTestFixture, FormattingFailureStillProducesAnErrorRecord)
{
    EXPECT_NO_THROW(Log::Info("{}", LoggerOwnershipTest::ThrowingValue{}));
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, "[FORMAT ERROR: deliberate formatter failure]");
}

TEST_F(LoggerOwnershipTestFixture, LargeArgumentsHaveNoDeferredCaptureLimit)
{
    const std::string value(100, 'a');
    Log::Info("{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}", value, value, value, value, value, value, value,
              value, value, value, value, value, value, value, value, value);
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].Message, std::string(1600, 'a'));
}

TEST_F(LoggerOwnershipTestFixture, ConcurrentProducersKeepTheirOwnCallOrder)
{
    std::array<std::thread, 4> producers;
    for (unsigned producer = 0; producer < producers.size(); ++producer)
        producers[producer] = std::thread(
            [producer]
            {
                for (unsigned sequence = 0; sequence < 64; ++sequence)
                    Log::Info("{} {}", producer, sequence);
            });
    for (auto& producer : producers)
        producer.join();
    Log::Flush();
    const auto messages = Spy->GetMessages();
    ASSERT_EQ(messages.size(), 256u);
    std::array<unsigned, 4> next{};
    for (const auto& message : messages)
    {
        unsigned producer = 99, sequence = 99;
        ASSERT_EQ(std::sscanf(message.Message.c_str(), "%u %u", &producer, &sequence), 2);
        ASSERT_LT(producer, next.size());
        EXPECT_EQ(sequence, next[producer]++);
    }
    for (const auto count : next)
        EXPECT_EQ(count, 64u);
}

// Exit codes of the redirect child process.
constexpr int kDrainTookNoBatch = 2;
constexpr int kLineReachedNoSink = 3;
constexpr int kLineReachedAdoptedSink = 4;
constexpr std::chrono::seconds kChildTimeout{5};

// True when the drain thread took the queued records into its batch before the deadline.
bool WaitForDrainToTakeQueue(Log::LoggerState& state)
{
    const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard queueLock(state.QueueMutex);
            if (state.Queue.empty())
                return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// Parks the drain thread with one record, adopts a second state, then lets the drain thread dispatch.
// Exits 0 when the record reaches the sink of the state it was queued in.
[[noreturn]] void DispatchRecordQueuedBeforeRedirect(const CaptureSink& ownSink)
{
    Log::LoggerState& ownState = *Log::GetStateForSharing();
    static Log::LoggerState adoptedState;
    auto adoptedSink = std::make_unique<CaptureSink>();
    const CaptureSink& adoptedSpy = *adoptedSink;
    adoptedState.Sinks.push_back(std::move(adoptedSink));
    {
        std::unique_lock sinkLock(ownState.Mutex);
        Log::Info("queued before the redirect");
        if (!WaitForDrainToTakeQueue(ownState))
            std::_Exit(kDrainTookNoBatch);
        Log::RedirectToSharedState(&adoptedState);
    }
    // The drain thread writes under ownState.Mutex on both paths, so both sinks are read under it.
    const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard sinkLock(ownState.Mutex);
            if (adoptedSpy.GetCount() > 0)
                std::_Exit(kLineReachedAdoptedSink);
            if (ownSink.GetCount() > 0)
                std::_Exit(0);
        }
        std::this_thread::yield();
    }
    std::_Exit(kLineReachedNoSink);
}

// A drain thread serves the state that started it. A host that adopts another state later must not make it
// write its batch into the adopted sinks while it holds only its own state's lock.
TEST_F(LoggerOwnershipTestFixture, RecordQueuedBeforeARedirectReachesItsOwnSink)
{
    // The child must start a fresh process: a forked child has no drain thread.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(DispatchRecordQueuedBeforeRedirect(*Spy), ::testing::ExitedWithCode(0), "");
}
} // namespace
