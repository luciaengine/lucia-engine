// The editor's posted refresh: one refresh per burst of requests, none after its owner is destroyed,
// and a post the host drops releases the pending state.
#include <gtest/gtest.h>

#include "UI/PostedRefresh.h"

#include "UI/UIElement.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

using GameEngine::UIElement;
using GameEngine::EditorUI::PostedRefresh;

namespace
{

// UIElement::PostAction prefers the thread-local UI context's dispatcher, so a test installs this one
// with UiContextScope and drains it by hand.
class QueueDispatcher final : public GameEngine::UI::IUiDispatcher
{
public:
    bool Post(std::function<void()> action) override
    {
        if (RefusePosts)
            return false;
        ++PostCount;
        m_Queue.push_back(std::move(action));
        return true;
    }

    void Drain() override
    {
        std::vector<std::function<void()>> batch;
        batch.swap(m_Queue);
        for (auto& action : batch)
            if (action)
                action();
    }

    std::size_t PendingCount() const override { return m_Queue.size(); }

    bool RefusePosts = false;
    std::size_t PostCount = 0;

private:
    std::vector<std::function<void()>> m_Queue;
};

class PostedRefreshTest : public ::testing::Test
{
protected:
    QueueDispatcher dispatcher;
    GameEngine::UI::UiContextScope scope{&dispatcher, nullptr};
    UIElement host;
    int refreshCount = 0;
};

} // namespace

TEST_F(PostedRefreshTest, RequestsBeforeTheDrainRunTheRefreshOnce)
{
    PostedRefresh refresh(host, [this]() { ++refreshCount; });

    refresh.Request();
    refresh.Request();
    refresh.Request();
    EXPECT_EQ(dispatcher.PostCount, 1u);
    EXPECT_TRUE(refresh.IsPending());

    dispatcher.Drain();
    EXPECT_EQ(refreshCount, 1);
    EXPECT_FALSE(refresh.IsPending());

    refresh.Request();
    dispatcher.Drain();
    EXPECT_EQ(refreshCount, 2);
}

TEST_F(PostedRefreshTest, ARefreshDrainedAfterItsOwnerIsDestroyedDoesNothing)
{
    auto refresh = std::make_unique<PostedRefresh>(host, [this]() { ++refreshCount; });
    refresh->Request();
    ASSERT_EQ(dispatcher.PendingCount(), 1u);

    refresh.reset();
    dispatcher.Drain();

    EXPECT_EQ(refreshCount, 0);
}

TEST_F(PostedRefreshTest, APostTheHostDropsReleasesThePendingState)
{
    PostedRefresh refresh(host, [this]() { ++refreshCount; });
    dispatcher.RefusePosts = true;

    refresh.Request();
    EXPECT_FALSE(refresh.IsPending());

    dispatcher.RefusePosts = false;
    refresh.Request();
    dispatcher.Drain();
    EXPECT_EQ(refreshCount, 1);
}
