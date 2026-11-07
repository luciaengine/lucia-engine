#pragma once

#include "ECS/ECS.h"

#include <functional>

namespace GameEngine
{
class UIElement;
namespace Editor
{
class EditorChangeNotifications;
class UndoRedoService;
} // namespace Editor

/// Adds an image preset picker with one undo step per selection.
/// Requests an inspector refresh after applying a preset. Shows Custom when look values differ.
void AddTerrainGrassPresetRow(UIElement* parent, ECS::World* world, ECS::EntityHandle terrain,
                              Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo,
                              std::function<void()> requestRefresh);

} // namespace GameEngine
