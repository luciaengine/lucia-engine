#pragma once

#include "DayCycle/DayCycleAxis.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace GameEngine
{
class AssetManager;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor
{
class DayCycleWorkingCopy;
class UndoRedoService;

/// The controls the Day Cycle window's toolbar and the profile's inspector share. Each builds into
/// `parent`, which owns what it adds.

/// The axis toggle: two buttons, "Sun's elevation" and "Time of day", the leading one marked
/// active. `onChoose` is called with the view clicked.
void AddDayCycleAxisToggle(UIElement& parent, DayCycleAxisView view, std::function<void(DayCycleAxisView)> onChoose);

/// Adds Save, or Save a copy to the project for shipped assets, with status text.
/// A saved copy becomes the window's working copy and calls `onCopied` with its path.
/// Returns the group containing the status and button.
UIElement& AddDayCycleSaveButton(UIElement& parent, const std::shared_ptr<DayCycleWorkingCopy>& profile,
                           std::function<void(const std::filesystem::path&)> onCopied);

/// A dropdown of shipped presets. Loading tracks is one undo step; the profile keeps its name and description.
void AddDayCyclePresets(UIElement& parent, AssetManager& assets, const std::shared_ptr<DayCycleWorkingCopy>& profile,
                        UndoRedoService* undo);

} // namespace GameEngine::Editor
