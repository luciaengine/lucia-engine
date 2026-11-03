// Fixed-id editor commands (UI replay, execute_command) are registered by the
// controller that owns the feature, through EditorMenuRegistry. These arms pin
// that each owner's RegisterCommands makes its ids reachable through
// EditorMenuRegistry::InvokeCommand, the one dispatch EditorApplication runs.
//
// The registry is process-wide and keeps what it is given. Each arm replaces its
// commands with inert ones before the owner under test is destroyed, so no later
// test in this process can reach a dangling owner.

#include <gtest/gtest.h>

#include "Automation/UiReplayCommandIds.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "PlayMode/PlayModeManager.h"
#include "Scene/SceneEditorController.h"

#include <cstdint>
#include <initializer_list>
#include <string>

namespace
{
using namespace GameEngine;

void ReplaceWithInertCommands(std::initializer_list<std::uint32_t> commandIds)
{
    for (const std::uint32_t commandId : commandIds)
        Editor::EditorMenuRegistry::Get().RegisterCommand({commandId, [](std::string*) { return true; }});
}

TEST(OwnerRegisteredCommands, SceneEditorControllerRegistersSaveScene)
{
    Editor::SceneEditorController sceneEditor;
    sceneEditor.RegisterCommands();

    // An unattached controller has no document, so the save must reach it and fail
    // with its own reason rather than report the id as unknown.
    std::string error;
    EXPECT_EQ(Editor::EditorMenuRegistry::Get().InvokeCommand(UiReplayCommandIds::SaveScene, &error),
              Editor::EditorCommandResult::Failed);
    EXPECT_EQ(error, "scene editor not attached");

    ReplaceWithInertCommands({UiReplayCommandIds::SaveScene});
}

// Only the arms that leave Edit untouched: entering play needs a live engine.
TEST(OwnerRegisteredCommands, PlayModeManagerRegistersPlayCommandsAndAsserts)
{
    Editor::PlayModeManager playMode;
    playMode.RegisterCommands();
    auto& registry = Editor::EditorMenuRegistry::Get();

    std::string error;
    EXPECT_EQ(registry.InvokeCommand(UiReplayCommandIds::AssertPlayModeEdit, &error),
              Editor::EditorCommandResult::Succeeded);
    EXPECT_EQ(registry.InvokeCommand(UiReplayCommandIds::AssertPlayModeNotRunning, &error),
              Editor::EditorCommandResult::Succeeded);

    EXPECT_EQ(registry.InvokeCommand(UiReplayCommandIds::AssertPlayModePlay, &error),
              Editor::EditorCommandResult::Failed);
    EXPECT_EQ(error, "expected Play, got Edit");

    error.clear();
    EXPECT_EQ(registry.InvokeCommand(UiReplayCommandIds::PlayTogglePause, &error),
              Editor::EditorCommandResult::Failed);
    EXPECT_EQ(error, "PlayTogglePause requires Play or Paused state");

    EXPECT_EQ(registry.InvokeCommand(UiReplayCommandIds::PlayStop, &error), Editor::EditorCommandResult::Succeeded)
        << "stop in Edit is a no-op, not a refusal";
    EXPECT_EQ(playMode.GetState(), Editor::PlayModeState::Edit);

    ReplaceWithInertCommands({UiReplayCommandIds::PlayEnter, UiReplayCommandIds::PlayTogglePause,
                              UiReplayCommandIds::PlayStop, UiReplayCommandIds::AssertPlayModeEdit,
                              UiReplayCommandIds::AssertPlayModePlay, UiReplayCommandIds::AssertPlayModePaused,
                              UiReplayCommandIds::AssertPlayModeNotRunning});
}

} // namespace
