#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "Core/DeferredActionQueue.h"
#include "EditorPanelIds.h"
#include "PlayMode/PlayModeGameViewFocus.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Layout/Docking.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

using namespace GameEngine;
using GameEngine::Editor::PlayModeGameViewFocus;
using GameEngine::Editor::PlayModeState;

namespace
{

constexpr const char* kHierarchyTree = "HierarchyTree";
constexpr const char* kInspectorField = "InspectorField";
constexpr const char* kSceneViewport = "SceneViewport";

// One editor window: a dock group with the tabs each test names, and the Inspector in a second
// group.
class PlayModeGameViewFocusTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        UIRegistration::RegisterBuiltInControls();
        AddPanelContent(m_Hierarchy, kHierarchyTree);
        AddPanelContent(m_GameView, EditorPanelIds::GameViewViewport);
        AddPanelContent(m_Inspector, kInspectorField);
        AddPanelContent(m_SceneView, kSceneViewport);
        m_Docking.RegisterPanel(EditorPanelIds::Hierarchy, &m_Hierarchy);
        m_Docking.RegisterPanel(EditorPanelIds::GameView, &m_GameView);
        m_Docking.RegisterPanel(EditorPanelIds::Inspector, &m_Inspector);
        m_Docking.RegisterPanel(EditorPanelIds::SceneView, &m_SceneView);

        auto root = std::make_unique<UIElement>();
        auto dockspace = std::make_unique<DockspaceElement>();
        dockspace->SetId("dock");
        dockspace->BindModel(&m_Docking);
        m_Dockspace = dockspace.get();
        root->AddChild(std::move(dockspace));
        m_Ui.SetRoot(std::move(root));
    }

    // The left group holds `leftTabs` with `active` in front; the right group holds the Inspector.
    void SetLayout(std::initializer_list<const char*> leftTabs, const char* active)
    {
        SetLayoutModel(leftTabs, active);
        m_Dockspace->RebuildFromModel();
    }

    // The model only: the dock tree remounts at the next RebuildFromModel, as at a UI update.
    void SetLayoutModel(std::initializer_list<const char*> leftTabs, const char* active)
    {
        auto left = DockNode::MakeLeaf();
        for (const char* tab : leftTabs)
            left->AddTab(tab);
        (void)left->ActivateTab(active);
        auto right = DockNode::MakeLeaf();
        right->AddTab(EditorPanelIds::Inspector);
        auto split = DockNode::MakeSplit(DockPosition::Left);
        split->SetSplit(DockPosition::Left, 0.5f, std::move(left), std::move(right));
        m_Docking.SetRoot(std::move(split));
    }

    void ChangeState(PlayModeState state) { m_Focus.OnPlayModeStateChanged(state); }

    void EnterPlay()
    {
        ChangeState(PlayModeState::EnteringPlay);
        ChangeState(PlayModeState::Play);
    }

    void StopPlay()
    {
        ChangeState(PlayModeState::ExitingPlay);
        ChangeState(PlayModeState::Edit);
        m_AfterUiUpdate.FlushOnce();
    }

    bool IsMounted(const char* id) const { return m_Ui.GetRootElement()->FindById(HashStringId(id)) != nullptr; }

    const std::string& FocusedId() const { return m_Ui.GetFocusedElementId(); }

    static void AddPanelContent(DockPanel& panel, const char* id)
    {
        auto content = std::make_unique<UIElement>();
        content->SetId(id);
        panel.AddChild(std::move(content));
    }

    // The docked half of EditorPanelManager::ActivateOpenPanel: this window has no floating frames.
    bool ActivateOpenPanel(const std::string& panelId)
    {
        if (!m_Docking.ActivateTab(panelId))
            return false;
        (void)m_Dockspace->RequestActivationPatch(panelId);
        return true;
    }

    void VisitWindows(const PlayModeGameViewFocus::WindowVisitor& visit)
    {
        visit(m_Ui, m_Docking, [this](const std::string& panelId) { return ActivateOpenPanel(panelId); });
    }

    DockPanel m_Hierarchy{"Hierarchy"};
    DockPanel m_GameView{"Game"};
    DockPanel m_Inspector{"Inspector"};
    DockPanel m_SceneView{"Scene"};
    DockingManager m_Docking;
    UIManager m_Ui{nullptr};
    DockspaceElement* m_Dockspace = nullptr;
    DeferredActionQueue m_AfterUiUpdate;
    PlayModeGameViewFocus m_Focus{[this](const PlayModeGameViewFocus::WindowVisitor& visit) { VisitWindows(visit); },
                                  m_AfterUiUpdate};
};

} // namespace

TEST_F(PlayModeGameViewFocusTests, PlayGivesTheVisibleGameViewTheFocus)
{
    SetLayout({EditorPanelIds::GameView}, EditorPanelIds::GameView);
    m_Ui.SetFocusById(kInspectorField);

    EnterPlay();

    EXPECT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);
}

TEST_F(PlayModeGameViewFocusTests, PlayBringsAHiddenGameViewTabToTheFrontAndFocusesIt)
{
    SetLayout({EditorPanelIds::Hierarchy, EditorPanelIds::GameView}, EditorPanelIds::Hierarchy);
    m_Ui.SetFocusById(kHierarchyTree);
    ASSERT_FALSE(m_Docking.IsPanelActiveTab(EditorPanelIds::GameView));

    EnterPlay();

    EXPECT_TRUE(m_Docking.IsPanelActiveTab(EditorPanelIds::GameView));
    EXPECT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);
}

TEST_F(PlayModeGameViewFocusTests, PlayWithNoGameViewOpenLeavesTheFocusAndOpensNothing)
{
    SetLayout({EditorPanelIds::Hierarchy}, EditorPanelIds::Hierarchy);
    m_Ui.SetFocusById(kHierarchyTree);

    EnterPlay();

    EXPECT_EQ(FocusedId(), kHierarchyTree);
    EXPECT_FALSE(m_Docking.IsPanelActiveTab(EditorPanelIds::GameView));
}

TEST_F(PlayModeGameViewFocusTests, StopGivesTheFocusBackToTheElementThatHadIt)
{
    SetLayout({EditorPanelIds::GameView}, EditorPanelIds::GameView);
    m_Ui.SetFocusById(kInspectorField);
    EnterPlay();
    ASSERT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);

    StopPlay();

    EXPECT_EQ(FocusedId(), kInspectorField);
}

TEST_F(PlayModeGameViewFocusTests, StopGivesTheFocusToTheSceneViewWhenTheEarlierElementIsGone)
{
    SetLayout({EditorPanelIds::GameView}, EditorPanelIds::GameView);
    m_Ui.SetFocusById(kInspectorField);
    EnterPlay();
    ASSERT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);
    ASSERT_TRUE(m_Docking.RemoveTab(EditorPanelIds::Inspector));
    m_Dockspace->RebuildFromModel();

    StopPlay();

    EXPECT_EQ(FocusedId(), "");
}

TEST_F(PlayModeGameViewFocusTests, StopBringsBackTheTabThatWasInFrontAtPlay)
{
    SetLayout({EditorPanelIds::Hierarchy, EditorPanelIds::GameView}, EditorPanelIds::Hierarchy);
    m_Ui.SetFocusById(kHierarchyTree);
    EnterPlay();
    ASSERT_TRUE(m_Docking.IsPanelActiveTab(EditorPanelIds::GameView));

    StopPlay();

    EXPECT_TRUE(m_Docking.IsPanelActiveTab(EditorPanelIds::Hierarchy));
    EXPECT_EQ(FocusedId(), kHierarchyTree);
}

TEST_F(PlayModeGameViewFocusTests, ATabTheUserPicksDuringPlayStaysInFrontAtStop)
{
    SetLayout({EditorPanelIds::SceneView, EditorPanelIds::Hierarchy, EditorPanelIds::GameView},
              EditorPanelIds::SceneView);
    EnterPlay();
    ASSERT_TRUE(m_Docking.IsPanelActiveTab(EditorPanelIds::GameView));
    ASSERT_TRUE(ActivateOpenPanel(EditorPanelIds::Hierarchy));

    StopPlay();

    EXPECT_TRUE(m_Docking.IsPanelActiveTab(EditorPanelIds::Hierarchy));
    EXPECT_FALSE(m_Docking.IsPanelActiveTab(EditorPanelIds::SceneView));
}

TEST_F(PlayModeGameViewFocusTests, AFocusTheUserMovesDuringPlayIsKeptAtStop)
{
    SetLayout({EditorPanelIds::GameView}, EditorPanelIds::GameView);
    m_Ui.SetFocusById(kHierarchyTree);
    EnterPlay();
    ASSERT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);
    m_Ui.SetFocusById(kInspectorField);

    StopPlay();

    EXPECT_EQ(FocusedId(), kInspectorField);
}

TEST_F(PlayModeGameViewFocusTests, ResumeFromPauseKeepsTheFocusTheUserMoved)
{
    SetLayout({EditorPanelIds::GameView}, EditorPanelIds::GameView);
    EnterPlay();
    ASSERT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);
    ChangeState(PlayModeState::Paused);
    m_Ui.SetFocusById(kInspectorField);

    ChangeState(PlayModeState::Play);

    EXPECT_EQ(FocusedId(), kInspectorField);
}

TEST_F(PlayModeGameViewFocusTests, StopIntoChangeReviewRestoresTheFocusOnce)
{
    SetLayout({EditorPanelIds::GameView}, EditorPanelIds::GameView);
    m_Ui.SetFocusById(kInspectorField);
    EnterPlay();
    ASSERT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);

    ChangeState(PlayModeState::ExitingPlay);
    ChangeState(PlayModeState::ChangeReview);
    m_AfterUiUpdate.FlushOnce();
    ASSERT_EQ(FocusedId(), kInspectorField);
    m_Ui.SetFocusById(EditorPanelIds::GameViewViewport);
    ChangeState(PlayModeState::Edit);
    m_AfterUiUpdate.FlushOnce();

    EXPECT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);
}

TEST_F(PlayModeGameViewFocusTests, StopRestoresTheFocusAfterTheLayoutBeforePlayFullscreenIsBack)
{
    SetLayout({EditorPanelIds::Hierarchy, EditorPanelIds::GameView}, EditorPanelIds::Hierarchy);
    m_Ui.SetFocusById(kHierarchyTree);
    // Play fullscreen shows a layout with only the Game View, mounted at the next UI update.
    SetLayoutModel({EditorPanelIds::GameView}, EditorPanelIds::GameView);
    EnterPlay();
    m_Dockspace->RebuildFromModel();
    ASSERT_EQ(FocusedId(), EditorPanelIds::GameViewViewport);

    // Stop gives back the layout from before Play; the Hierarchy mounts at the next UI update.
    ChangeState(PlayModeState::ExitingPlay);
    ChangeState(PlayModeState::Edit);
    SetLayoutModel({EditorPanelIds::Hierarchy, EditorPanelIds::GameView}, EditorPanelIds::Hierarchy);
    ASSERT_FALSE(IsMounted(kHierarchyTree));
    m_Dockspace->RebuildFromModel();
    m_AfterUiUpdate.FlushOnce();

    EXPECT_EQ(FocusedId(), kHierarchyTree);
}
