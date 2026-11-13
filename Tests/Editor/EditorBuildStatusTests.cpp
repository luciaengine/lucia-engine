// The status bar's left corner: the build configuration, then the editor version with its commit,
// both in the tree from construction. The layout file is staged beside the suite.

#include <gtest/gtest.h>

#include "Editor/Application/BuildConfigStatus.h"
#include "Editor/Application/EditorVersion.h"
#include "UI/Controls/EditorBuildStatus.h"
#include "UI/Controls/Label.h"

using GameEngine::EditorBuildStatus;
using GameEngine::Label;
using GameEngine::operator""_sid;

namespace
{

TEST(EditorBuildStatusTests, ShowsTheBuildConfigurationThenTheEditorVersion)
{
    EditorBuildStatus status;
    Label* configuration = status.FindById<Label>("BuildConfigStatus"_sid).Get();
    Label* version = status.FindById<Label>("EditorVersionStatus"_sid).Get();
    ASSERT_NE(configuration, nullptr);
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(configuration->GetText(), GameEngine::Editor::BuildConfigStatusText());
    EXPECT_EQ(version->GetText(), GameEngine::Editor::EditorVersionText());
    EXPECT_FALSE(version->GetText().empty());
}

} // namespace
