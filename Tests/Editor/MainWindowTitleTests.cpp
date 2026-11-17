// The main window's titles: the caption row reads the scene, its unsaved mark and the project and
// nothing else; the operating system's title starts with the application name and adds a known session.

#include <gtest/gtest.h>

#include "Editor/Application/MainWindowTitle.h"

using GameEngine::Editor::MainWindowTitle;

namespace
{

TEST(MainWindowTitleTests, TheWindowTitleNamesTheEngineBeforeTheSceneAndTheProject)
{
    const MainWindowTitle title = MainWindowTitle::Format("Harbor", false, "Coastline", "");
    EXPECT_EQ(title.Caption, "Harbor - Coastline");
    EXPECT_EQ(title.WindowTitle, "Lucia Engine - Harbor - Coastline");
}

TEST(MainWindowTitleTests, OnlyTheWindowTitleNamesTheSession)
{
    const MainWindowTitle title = MainWindowTitle::Format("Harbor", false, "Coastline", "wt-staging@main");
    EXPECT_EQ(title.Caption, "Harbor - Coastline");
    EXPECT_EQ(title.WindowTitle, "Lucia Engine - Harbor - Coastline  [wt-staging@main]");
}

TEST(MainWindowTitleTests, AnUnsavedSceneCarriesTheMarkInBothTitles)
{
    const MainWindowTitle title = MainWindowTitle::Format("Untitled", true, "Coastline", "wt-staging");
    EXPECT_EQ(title.Caption, "Untitled* - Coastline");
    EXPECT_EQ(title.WindowTitle, "Lucia Engine - Untitled* - Coastline  [wt-staging]");
}

TEST(MainWindowTitleTests, MissingPartsLeaveNoSeparator)
{
    EXPECT_EQ(MainWindowTitle::Format("", false, "Coastline", "").Caption, "Coastline");
    EXPECT_EQ(MainWindowTitle::Format("Harbor", false, "", "").Caption, "Harbor");
    EXPECT_EQ(MainWindowTitle::Format("", false, "", "").WindowTitle, "Lucia Engine");
    EXPECT_EQ(MainWindowTitle::Format("", false, "", "wt-staging").WindowTitle, "Lucia Engine  [wt-staging]");
}

} // namespace
