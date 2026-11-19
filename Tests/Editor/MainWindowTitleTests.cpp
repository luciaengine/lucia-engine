#include <gtest/gtest.h>

#include "Editor/Application/MainWindowTitle.h"

using GameEngine::Editor::MainWindowTitle;

namespace
{

TEST(MainWindowTitleTests, BothTitlesNameLuciaThenTheProjectThenTheScene)
{
    const MainWindowTitle title = MainWindowTitle::Format("Harbor", false, "Coastline", "");
    EXPECT_EQ(title.Caption, "Lucia Engine - Coastline - Harbor");
    EXPECT_EQ(title.WindowTitle, "Lucia Engine - Coastline - Harbor");
}

TEST(MainWindowTitleTests, OnlyTheWindowTitleNamesTheSession)
{
    const MainWindowTitle title = MainWindowTitle::Format("Harbor", false, "Coastline", "wt-staging@main");
    EXPECT_EQ(title.Caption, "Lucia Engine - Coastline - Harbor");
    EXPECT_EQ(title.WindowTitle, "Lucia Engine - Coastline - Harbor  [wt-staging@main]");
}

TEST(MainWindowTitleTests, AnUnsavedSceneCarriesTheMarkInBothTitles)
{
    const MainWindowTitle title = MainWindowTitle::Format("Untitled", true, "Coastline", "wt-staging");
    EXPECT_EQ(title.Caption, "Lucia Engine - Coastline - Untitled*");
    EXPECT_EQ(title.WindowTitle, "Lucia Engine - Coastline - Untitled*  [wt-staging]");
}

TEST(MainWindowTitleTests, MissingPartsLeaveNoSeparator)
{
    EXPECT_EQ(MainWindowTitle::Format("", false, "Coastline", "").Caption, "Lucia Engine - Coastline");
    EXPECT_EQ(MainWindowTitle::Format("Harbor", false, "", "").Caption, "Lucia Engine - Harbor");
    EXPECT_EQ(MainWindowTitle::Format("", false, "", "").WindowTitle, "Lucia Engine");
    EXPECT_EQ(MainWindowTitle::Format("", false, "", "wt-staging").WindowTitle, "Lucia Engine  [wt-staging]");
}

} // namespace
