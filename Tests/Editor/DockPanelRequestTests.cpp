#include "DebugServer/DockPanelRequest.h"

#include "UI/Layout/Docking.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace
{
using namespace GameEngine;
using Editor::DockPanelRequest;
using json = nlohmann::json;

void SetUpDock(DockingManager& docking)
{
    docking.RegisterPanel("Assets", nullptr);
    docking.RegisterPanel("Inspector", nullptr);
    docking.RegisterPanel("Hierarchy", nullptr);
    auto root = DockNode::MakeSplit(DockPosition::Left, 0.4f);
    root->First()->AddTab("Assets");
    root->Second()->AddTab("Hierarchy");
    root->Second()->AddTab("Inspector");
    docking.SetRoot(std::move(root));
}

TEST(DockPanelRequestTests, InvalidIdentifiersAreRejectedBeforePlacement)
{
    for (const json& params : {json::object(), json::array(), json{{"panelId", 1}},
                              json{{"panelId", ""}}, json{{"panelId", "Assets"}, {"anchorPanelId", false}}})
        EXPECT_FALSE(DockPanelRequest(params).Error().empty());
}

TEST(DockPanelRequestTests, UnknownPanelNeverInvokesOpeningPolicy)
{
    DockingManager docking;
    SetUpDock(docking);
    DockPanelRequest request(json{{"panelId", "Unknown"}});
    EXPECT_FALSE(request.Apply(docking, [](const auto&, const auto&) { FAIL() << "Unknown panel opened"; }));
    EXPECT_FALSE(request.Error().empty());
    EXPECT_TRUE(docking.GetRoot()->IsSplit());
}

TEST(DockPanelRequestTests, OpeningPolicyThatLeavesPanelFloatingDoesNotDuplicateItInTheDock)
{
    DockingManager docking;
    SetUpDock(docking);
    docking.RemoveTab("Inspector");
    DockPanelRequest request(json{{"panelId", "Inspector"}, {"anchorPanelId", "Hierarchy"}});
    unsigned calls = 0;
    EXPECT_FALSE(request.Apply(docking, [&](const auto&, const auto&) { ++calls; }));
    EXPECT_EQ(calls, 1u);
    std::string leaf;
    EXPECT_EQ(docking.FindLeafContaining("Inspector", leaf), nullptr);
    const std::vector<std::string> expected{"Hierarchy"};
    EXPECT_EQ(docking.GetRoot()->Second()->GetTabs(), expected);
    EXPECT_FALSE(request.Error().empty());
}

TEST(DockPanelRequestTests, InvalidAnchorLeavesTheSourceTabAndLayoutIntact)
{
    DockingManager docking;
    SetUpDock(docking);
    DockPanelRequest request(json{{"panelId", "Assets"}, {"anchorPanelId", "Missing"}});
    bool opened = false;
    EXPECT_FALSE(request.Apply(docking, [&opened](const auto&, const auto&) { opened = true; }));
    EXPECT_FALSE(opened);
    std::string leaf;
    EXPECT_NE(docking.FindLeafContaining("Assets", leaf), nullptr);
    EXPECT_EQ(leaf, "0");
    EXPECT_TRUE(docking.GetRoot()->IsSplit());
}

TEST(DockPanelRequestTests, MovingTheLastTabRequeriesAnchorAfterSplitCollapse)
{
    DockingManager docking;
    SetUpDock(docking);
    DockPanelRequest request(json{{"panelId", "Assets"}, {"anchorPanelId", "Inspector"}});
    ASSERT_TRUE(request.Apply(docking, [](const auto&, const auto&) { FAIL() << "Already docked"; }));
    std::string sourceLeaf;
    std::string anchorLeaf;
    ASSERT_NE(docking.FindLeafContaining("Assets", sourceLeaf), nullptr);
    ASSERT_NE(docking.FindLeafContaining("Inspector", anchorLeaf), nullptr);
    EXPECT_EQ(sourceLeaf, anchorLeaf);
    EXPECT_TRUE(docking.IsPanelActiveTab("Assets"));
    EXPECT_EQ(request.Report(docking)["movedFrom"], "0");
    const std::vector<std::string> expected{"Hierarchy", "Inspector", "Assets"};
    EXPECT_EQ(docking.GetRoot()->GetTabs(), expected);
    DockPanelRequest repeated(json{{"panelId", "Assets"}, {"anchorPanelId", "Inspector"}});
    ASSERT_TRUE(repeated.Apply(docking, [](const auto&, const auto&) { FAIL() << "Already docked"; }));
    EXPECT_EQ(docking.GetRoot()->GetTabs(), expected);
}

TEST(DockPanelRequestTests, NoAnchorPreservesPlacementAndTabOrder)
{
    DockingManager docking;
    SetUpDock(docking);
    DockPanelRequest request(json{{"panelId", "Inspector"}});
    const auto before = docking.GetRoot()->Second()->GetTabs();
    ASSERT_TRUE(request.Apply(docking, [](const auto&, const auto&) { FAIL() << "Already docked"; }));
    EXPECT_EQ(docking.GetRoot()->Second()->GetTabs(), before);
    EXPECT_FALSE(request.Report(docking).contains("movedFrom"));
    EXPECT_EQ(request.Report(docking)["leaf"], "1");
}

TEST(DockPanelRequestTests, SameLeafAndSelfAnchorDoNotReorderTabs)
{
    for (const auto* anchor : {"Inspector", "Hierarchy"})
    {
        DockingManager docking;
        SetUpDock(docking);
        const auto before = docking.GetRoot()->Second()->GetTabs();
        DockPanelRequest request(json{{"panelId", "Inspector"}, {"anchorPanelId", anchor}});
        ASSERT_TRUE(request.Apply(docking, [](const auto&, const auto&) { FAIL() << "Already docked"; }));
        EXPECT_EQ(docking.GetRoot()->Second()->GetTabs(), before);
        EXPECT_FALSE(request.Report(docking).contains("movedFrom"));
    }
}

TEST(DockPanelRequestTests, ClosedPanelUsesTheExistingOpeningPolicy)
{
    DockingManager docking;
    SetUpDock(docking);
    docking.RemoveTab("Inspector");
    DockPanelRequest request(json{{"panelId", "Inspector"}});
    bool opened = false;
    ASSERT_TRUE(request.Apply(docking, [&](const std::string& panel, const std::string& anchor) {
        opened = true;
        EXPECT_EQ(panel, "Inspector");
        EXPECT_TRUE(anchor.empty());
        docking.DockAsTabInLeafByPath("1", panel);
    }));
    EXPECT_TRUE(opened);
}

TEST(DockPanelRequestTests, ReadbackUsesCurrentPlacementRatherThanTheApplySnapshot)
{
    DockingManager docking;
    SetUpDock(docking);
    DockPanelRequest request(json{{"panelId", "Assets"}});
    ASSERT_TRUE(request.Apply(docking, [](const auto&, const auto&) {}));
    docking.RemoveTab("Assets");
    EXPECT_FALSE(request.Report(docking)["docked"].get<bool>());
}
} // namespace
