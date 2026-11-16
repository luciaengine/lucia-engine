#include "EditorPanelIds.h"

#include <gtest/gtest.h>


namespace GameEngine
{
namespace
{

TEST(EditorPanelDefaultPlacement, SettingsOpensBesideTheSceneView)
{
    EXPECT_STREQ(EditorPanelIds::DefaultPanelLeafAnchor(EditorPanelIds::Settings), EditorPanelIds::SceneView);
}

TEST(EditorPanelDefaultPlacement, ProjectToolsStayBesideTheHierarchy)
{
    EXPECT_STREQ(EditorPanelIds::DefaultPanelLeafAnchor(EditorPanelIds::Todo), EditorPanelIds::Hierarchy);
    EXPECT_STREQ(EditorPanelIds::DefaultPanelLeafAnchor(EditorPanelIds::Bookmarks), EditorPanelIds::Hierarchy);
}

} // namespace
} // namespace GameEngine
