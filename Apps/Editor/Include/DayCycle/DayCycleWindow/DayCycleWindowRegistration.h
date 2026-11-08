#pragma once

#include "ECS/Entity.h"

#include <functional>
#include <vector>

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// The Day Cycle window's dock id.
inline constexpr const char* kDayCycleWindowPanelId = "DayCycle";

/// Registers the Day Cycle panel with editor undo, selection, and playback services.
/// ProjectSwitched closes all profile working copies.
void RegisterDayCycleWindow(UndoRedoService* undo, EditorChangeNotifications& notifications,
                            std::function<std::vector<ECS::EntityHandle>()> selection,
                            std::function<bool()> isPlaying);

} // namespace GameEngine::Editor
