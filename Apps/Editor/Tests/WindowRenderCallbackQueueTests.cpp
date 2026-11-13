#include "DebugServer/WindowRenderCallbackQueue.h"

#include <gtest/gtest.h>

namespace GameEngine::Editor
{
TEST(WindowRenderCallbackQueue, SecondaryDeclarationWaitsForItsOwnSeam)
{
    WindowRenderCallbackQueue queue;
    uint32_t oldSecondaryPasses = 5;
    uint32_t freshSecondaryPasses = 0;
    uint32_t* secondaryGraph = &oldSecondaryPasses;
    auto callback = std::make_shared<WindowRenderCallbackQueue::Callback>(
        [&secondaryGraph](uint64_t windowId)
        {
            EXPECT_EQ(windowId, 22u);
            ++*secondaryGraph;
        });
    queue.Enqueue(22, callback);
    EXPECT_FALSE(queue.HasPending(11));
    ASSERT_TRUE(queue.HasPending(22));
    queue.Dispatch(11);
    EXPECT_EQ(oldSecondaryPasses, 5u);
    ASSERT_TRUE(queue.HasPending(22));
    secondaryGraph = &freshSecondaryPasses;
    queue.Dispatch(22);
    EXPECT_EQ(freshSecondaryPasses, 1u);
    EXPECT_EQ(oldSecondaryPasses, 5u);
    EXPECT_FALSE(queue.HasPending(22));
    queue.Dispatch(22);
    EXPECT_EQ(freshSecondaryPasses, 1u);
}

TEST(WindowRenderCallbackQueue, BothTargetsDispatchIndependently)
{
    WindowRenderCallbackQueue queue;
    std::vector<uint64_t> observed;
    auto callback = std::make_shared<WindowRenderCallbackQueue::Callback>(
        [&observed](uint64_t windowId) { observed.push_back(windowId); });
    queue.Enqueue(11, callback);
    queue.Enqueue(22, callback);
    queue.Dispatch(22);
    EXPECT_EQ(observed, (std::vector<uint64_t>{22}));
    EXPECT_TRUE(queue.HasPending(11));
    queue.Dispatch(11);
    EXPECT_EQ(observed, (std::vector<uint64_t>{22, 11}));
}

TEST(WindowRenderCallbackQueue, EndingResponseCancelsQueuedDeclaration)
{
    WindowRenderCallbackQueue queue;
    bool called = false;
    auto callback = std::make_shared<WindowRenderCallbackQueue::Callback>(
        [&called](uint64_t) { called = true; });
    queue.Enqueue(22, callback);
    callback.reset();
    EXPECT_FALSE(queue.HasPending(22));
    queue.Dispatch(22);
    EXPECT_FALSE(called);
}

TEST(WindowRenderCallbackQueue, ReplacementWindowCannotConsumeClosedWindowsRequest)
{
    WindowRenderCallbackQueue queue;
    bool called = false;
    auto callback = std::make_shared<WindowRenderCallbackQueue::Callback>(
        [&called](uint64_t) { called = true; });
    queue.Enqueue(22, callback);
    // The replacement may occupy the same vector slot; its ID is new.
    queue.Dispatch(23);
    EXPECT_FALSE(called);
    EXPECT_TRUE(queue.HasPending(22));
    callback.reset();
    queue.Dispatch(11);
    EXPECT_FALSE(queue.HasPending(22));
}

TEST(WindowRenderCallbackQueue, RequeuedWorkWaitsForTheNextSeam)
{
    WindowRenderCallbackQueue queue;
    uint32_t calls = 0;
    std::shared_ptr<WindowRenderCallbackQueue::Callback> callback;
    callback = std::make_shared<WindowRenderCallbackQueue::Callback>(
        [&queue, &calls, &callback](uint64_t windowId)
        {
            ++calls;
            queue.Enqueue(windowId, callback);
        });
    queue.Enqueue(22, callback);
    queue.Dispatch(22);
    EXPECT_EQ(calls, 1u);
    EXPECT_TRUE(queue.HasPending(22));
    queue.Dispatch(22);
    EXPECT_EQ(calls, 2u);
}
} // namespace GameEngine::Editor
