#include <gtest/gtest.h>

#include "Input/KeyCodes.h"
#include "UI/Controls/DockTab.h"
#include "UI/Controls/DockTabBar.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Layout/Docking.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr float kTabHeight = 24.0f;
constexpr float kTabWidth = 80.0f;
constexpr float kTabBX = 84.0f;

// Sends a press to the target and the release to the element the press captured,
// the way UIManager routes a captured pointer.
void Click(UIElement& target, int button, float downX, float downY, float upX, float upY)
{
    UIEvent down{};
    down.Id = kEventMouseDown;
    down.Button = button;
    down.X = downX;
    down.Y = downY;
    down.Target = &target;
    down.CurrentTarget = &target;
    target.DispatchEvent(down);

    UIElement* receiver = down.CaptureRequested ? down.CaptureRequested : &target;
    UIEvent up{};
    up.Id = kEventMouseUp;
    up.Button = button;
    up.X = upX;
    up.Y = upY;
    up.Target = receiver;
    up.CurrentTarget = receiver;
    receiver->DispatchEvent(up);
}

// Two tabs, A and B, in one leaf with a laid-out tab strip that does not overflow.
class DockTabCloseTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        UIRegistration::RegisterBuiltInControls();
        m_Model.RegisterPanel("A", &m_PanelA);
        m_Model.RegisterPanel("B", &m_PanelB);
        auto leaf = DockNode::MakeLeaf();
        leaf->AddTab("A");
        leaf->AddTab("B");
        m_Model.SetRoot(std::move(leaf));
        m_Dock.BindModel(&m_Model);
        m_Dock.RebuildFromModel();

        m_TabA = dynamic_cast<DockTab*>(m_Dock.FindById("tab:A"_sid));
        m_TabB = dynamic_cast<DockTab*>(m_Dock.FindById("tab:B"_sid));
        ASSERT_NE(m_TabA, nullptr);
        ASSERT_NE(m_TabB, nullptr);
        ASSERT_NE(dynamic_cast<DockTabBar*>(m_TabB->GetParent()), nullptr);
        UILayoutAccess::SetLastLayoutRect(*m_TabB->GetParent(), 0.0f, 0.0f, 400.0f, kTabHeight);
        UILayoutAccess::SetLastLayoutRect(*m_TabA, 0.0f, 0.0f, kTabWidth, kTabHeight);
        UILayoutAccess::SetLastLayoutRect(*m_TabB, kTabBX, 0.0f, kTabWidth, kTabHeight);
    }

    size_t TabCount() const { return m_Model.GetRoot()->GetTabs().size(); }

    UIElement m_PanelA{"panel-a"};
    UIElement m_PanelB{"panel-b"};
    DockingManager m_Model;
    DockspaceElement m_Dock;
    DockTab* m_TabA = nullptr;
    DockTab* m_TabB = nullptr;
};

} // namespace

TEST_F(DockTabCloseTests, MiddleClickClosesTheTabThroughTheHostCloseHandler)
{
    std::vector<std::string> closed;
    m_Dock.SetOnTabCloseRequested([&closed](const std::string& panelId) { closed.push_back(panelId); });

    const float x = kTabBX + kTabWidth * 0.5f;
    Click(*m_TabB, Input::kMouseButton_Middle, x, 12.0f, x + 2.0f, 13.0f);

    ASSERT_EQ(closed.size(), 1u);
    EXPECT_EQ(closed[0], "B");
}

TEST_F(DockTabCloseTests, MiddleClickWithoutAHostHandlerRemovesTheTab)
{
    const float x = kTabBX + kTabWidth * 0.5f;
    Click(*m_TabB, Input::kMouseButton_Middle, x, 12.0f, x, 12.0f);

    ASSERT_EQ(TabCount(), 1u);
    EXPECT_EQ(m_Model.GetRoot()->GetTabs()[0], "A");
    EXPECT_EQ(m_Dock.FindById("tab:B"_sid), nullptr);
}

TEST_F(DockTabCloseTests, MiddleDragDoesNotClose)
{
    int closeRequests = 0;
    m_Dock.SetOnTabCloseRequested([&closeRequests](const std::string&) { ++closeRequests; });

    const float x = kTabBX + kTabWidth * 0.5f;
    Click(*m_TabB, Input::kMouseButton_Middle, x, 12.0f, x + 20.0f, 12.0f);

    EXPECT_EQ(closeRequests, 0);
    EXPECT_EQ(TabCount(), 2u);
}

TEST_F(DockTabCloseTests, MiddleReleaseOverAnotherTabDoesNotClose)
{
    int closeRequests = 0;
    m_Dock.SetOnTabCloseRequested([&closeRequests](const std::string&) { ++closeRequests; });

    // The press is on B's left edge and the release on A's right edge, 4 px away: a click distance.
    Click(*m_TabB, Input::kMouseButton_Middle, kTabBX, 12.0f, kTabWidth, 12.0f);

    EXPECT_EQ(closeRequests, 0);
    EXPECT_EQ(TabCount(), 2u);
}

TEST_F(DockTabCloseTests, RightClickOpensTheContextMenuAndDoesNotClose)
{
    std::vector<std::string> menus;
    int closeRequests = 0;
    m_Dock.SetOnTabContextMenu([&menus](const std::string& panelId, float, float) { menus.push_back(panelId); });
    m_Dock.SetOnTabCloseRequested([&closeRequests](const std::string&) { ++closeRequests; });

    const float x = kTabBX + kTabWidth * 0.5f;
    Click(*m_TabB, Input::kMouseButton_Right, x, 12.0f, x, 12.0f);

    ASSERT_EQ(menus.size(), 1u);
    EXPECT_EQ(menus[0], "B");
    EXPECT_EQ(closeRequests, 0);
    EXPECT_EQ(TabCount(), 2u);
}

TEST(DockTabCloseOutsideDockspaceTests, MiddleClickOnATabWithNoDockspaceDoesNothing)
{
    UIRegistration::RegisterBuiltInControls();
    DockTabBar bar;
    auto tabOwner = std::make_unique<DockTab>();
    DockTab* tab = tabOwner.get();
    tab->SetId("tab:Lonely");
    tab->AddClass("tab");
    bar.AddTabChild(std::move(tabOwner));
    UILayoutAccess::SetLastLayoutRect(bar, 0.0f, 0.0f, 400.0f, kTabHeight);
    UILayoutAccess::SetLastLayoutRect(*tab, 0.0f, 0.0f, kTabWidth, kTabHeight);

    Click(*tab, Input::kMouseButton_Middle, 10.0f, 12.0f, 10.0f, 12.0f);

    EXPECT_EQ(bar.FindById("tab:Lonely"_sid), tab);
}

TEST_F(DockTabCloseTests, MiddleDragThatComesBackToThePressDoesNotClose)
{
    int closeRequests = 0;
    m_Dock.SetOnTabCloseRequested([&closeRequests](const std::string&) { ++closeRequests; });
    UIElement* bar = m_TabB->GetParent();

    const float x = kTabBX + kTabWidth * 0.5f;
    UIEvent down{};
    down.Id = kEventMouseDown;
    down.Button = Input::kMouseButton_Middle;
    down.X = x;
    down.Y = 12.0f;
    m_TabB->DispatchEvent(down);
    ASSERT_EQ(down.CaptureRequested, bar);

    UIEvent move{};
    move.Id = kEventMouseMove;
    move.X = x + 30.0f;
    move.Y = 12.0f;
    bar->DispatchEvent(move);

    UIEvent up{};
    up.Id = kEventMouseUp;
    up.Button = Input::kMouseButton_Middle;
    up.X = x;
    up.Y = 12.0f;
    bar->DispatchEvent(up);

    EXPECT_EQ(closeRequests, 0);
}

TEST_F(DockTabCloseTests, MiddleClickOnAScrollArrowDoesNotCloseTheTabUnderIt)
{
    int closeRequests = 0;
    m_Dock.SetOnTabCloseRequested([&closeRequests](const std::string&) { ++closeRequests; });
    UIElement* bar = m_TabB->GetParent();
    UILayoutAccess::SetLastLayoutRect(*bar, 0.0f, 0.0f, 120.0f, kTabHeight);
    UIElement* arrow = nullptr;
    for (const auto& child : bar->GetChildren())
    {
        if (child && child->HasClass("dock-tab-scroll-right"))
            arrow = child.get();
    }
    ASSERT_NE(arrow, nullptr);
    arrow->RemoveClass("hidden");
    UILayoutAccess::SetLastLayoutRect(*arrow, 100.0f, 0.0f, 20.0f, kTabHeight);

    // The arrow lies over the end of tab B. Its press bubbles to the tab bar.
    Click(*bar, Input::kMouseButton_Middle, 110.0f, 12.0f, 110.0f, 12.0f);

    EXPECT_EQ(closeRequests, 0);
    EXPECT_EQ(TabCount(), 2u);
}

TEST_F(DockTabCloseTests, CancelledMiddlePressClosesNothingOnALaterRelease)
{
    int closeRequests = 0;
    m_Dock.SetOnTabCloseRequested([&closeRequests](const std::string&) { ++closeRequests; });
    UIElement* bar = m_TabB->GetParent();

    const float x = kTabBX + kTabWidth * 0.5f;
    UIEvent down{};
    down.Id = kEventMouseDown;
    down.Button = Input::kMouseButton_Middle;
    down.X = x;
    down.Y = 12.0f;
    m_TabB->DispatchEvent(down);
    ASSERT_EQ(down.CaptureRequested, bar);

    UIEvent cancel{};
    cancel.Id = kEventMouseCancel;
    cancel.Button = Input::kMouseButton_Middle;
    bar->DispatchEvent(cancel);

    UIEvent up{};
    up.Id = kEventMouseUp;
    up.Button = Input::kMouseButton_Middle;
    up.X = x;
    up.Y = 12.0f;
    bar->DispatchEvent(up);

    EXPECT_EQ(closeRequests, 0);
    EXPECT_EQ(TabCount(), 2u);
}

TEST_F(DockTabCloseTests, CancelledMiddlePressStopsPanningAnOverflowingStrip)
{
    UIElement* bar = m_TabB->GetParent();
    UILayoutAccess::SetLastLayoutRect(*bar, 0.0f, 0.0f, 120.0f, kTabHeight);
    ASSERT_FALSE(m_TabA->Style().Get(Style::PositionLeft).has_value());

    UIEvent down{};
    down.Id = kEventMouseDown;
    down.Button = Input::kMouseButton_Middle;
    down.X = 40.0f;
    down.Y = 12.0f;
    m_TabA->DispatchEvent(down);
    ASSERT_EQ(down.CaptureRequested, bar);
    ASSERT_TRUE(bar->HasClass("middle-panning"));

    UIEvent cancel{};
    cancel.Id = kEventMouseCancel;
    cancel.Button = Input::kMouseButton_Middle;
    bar->DispatchEvent(cancel);
    EXPECT_FALSE(bar->HasClass("middle-panning"));

    // Without the press, a move over the strip does not scroll it.
    UIEvent move{};
    move.Id = kEventMouseMove;
    move.X = 10.0f;
    move.Y = 12.0f;
    bar->DispatchEvent(move);

    EXPECT_FALSE(m_TabA->Style().Get(Style::PositionLeft).has_value());
}
