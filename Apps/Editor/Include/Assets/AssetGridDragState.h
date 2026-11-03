#pragma once

#include <filesystem>

namespace GameEngine {

// The asset the Assets panel grid or list is dragging, for drop targets that
// accept a plain asset drag outside DragDropManager (Bookmarks, the web view,
// the Build settings slots). AssetsBrowserController sets it when a drag starts;
// the drop target that consumes it clears it.
struct DragState
{
    bool Active = false;
    std::filesystem::path AssetPath;
};

// Defined in AssetsBrowserController.cpp. UI thread only.
extern DragState g_DragState;

} // namespace GameEngine
